/*
 * The MIT License (MIT)

 * Copyright (c) 2015 Microsoft Corporation, Robust Distributed System Nucleus(rDSN)

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "replica.h"
#include "replica_stub.h"
#include "mutation_log.h"
#include "mutation.h"
#include "replication_failure_detector.h"
#include "rpc_replicated.h"
#include <boost/filesystem.hpp>

#define __TITLE__ "Stub"

namespace dsn { namespace replication {

using namespace dsn::service;

replica_stub::replica_stub(replica_state_subscriber subscriber /*= nullptr*/, bool is_long_subscriber/* = true*/)
    : serverlet("replica_stub")
{
    _replicaStateSubscriber = subscriber;
    _isLongSubscriber = is_long_subscriber;
    _failure_detector = nullptr;
    _state = NS_Disconnected;
}

replica_stub::~replica_stub(void)
{
    close();
}

void replica_stub::initialize(configuration_ptr config, bool clear/* = false*/)
{
    replication_options opts;
    opts.initialize(config);
    initialize(opts, config, clear);
}

void replica_stub::initialize(const replication_options& opts, configuration_ptr config, bool clear/* = false*/)
{
    zauto_lock l(_replicasLock);

    _config = config;

    // init perf counters
    //PerformanceCounters::init(PerfCounters_ReplicationBegin, PerfCounters_ReplicationEnd);

    // init dirs
    set_options(opts);
    _dir = _options.WorkingDir;
    if (clear)
    {
        boost::filesystem::remove_all(_dir);
    }

    if (!boost::filesystem::exists(_dir))
    {
        boost::filesystem::create_directory(_dir);
    }

    _dir = boost::filesystem::canonical(boost::filesystem::path(_dir)).string();
    std::string logDir = _dir + "/log";
    if (!boost::filesystem::exists(logDir))
    {
        boost::filesystem::create_directory(logDir);
    }

    // init replicas
    boost::filesystem::directory_iterator endtr;
    Replicas replicas;

    for (boost::filesystem::directory_iterator it(dir());
        it != endtr;
        ++it)
    {
        auto name = it->path().string();
        if (name.length() >= 4 &&
            (name.substr(name.length() - strlen("log")) == "log" ||
            name.substr(name.length() - strlen(".err")) == ".err")
            )
            continue;

        auto r = replica::load(this, name.c_str(), _options, true);
        if (r != nullptr)
        {
            ddebug( "%u.%u @ %s:%u: load replica success with durable decree = %llu from '%s'",
                r->get_gpid().app_id, r->get_gpid().pidx,
                address().name.c_str(), static_cast<int>(address().port),
                r->last_durable_decree(),
                name.c_str()
                );
            replicas[r->get_gpid()] = r;
        }
    }

    // init logs
    _log = new mutation_log(opts.LogBufferSizeMB, opts.LogPendingMaxMilliseconds, opts.LogSegmentFileSizeInMB, opts.LogBatchWrite, opts.LogMaxConcurrentWriteLogEntries);
    int err = _log->initialize(logDir.c_str());
    dassert (err == ERR_SUCCESS, "");
    
    err = _log->replay(
        std::bind(&replica_stub::replay_mutation, this, std::placeholders::_1, &replicas)
        );
    
    for (auto it = replicas.begin(); it != replicas.end(); it++)
    {
        it->second->reset_prepare_list_after_replay();

        derror(
            "%u.%u @ %s:%u: initialized durable = %lld, committed = %llu, maxpd = %llu, ballot = %llu",
            it->first.app_id, it->first.pidx,
            address().name.c_str(), static_cast<int>(address().port),
            it->second->last_durable_decree(),
            it->second->last_committed_decree(),
            it->second->max_prepared_decree(),
            it->second->get_ballot()
            );

        if (err != ERR_SUCCESS)
        {
            // prevent them to be primary, secondary, etc.
            it->second->update_local_configuration_with_no_ballot_change(PS_ERROR);
        }
        else
        {
            it->second->set_inactive_state_transient(true);
        }
    }

    // start log serving    
    if (false == _options.GcDisabled)
    {
        _gcTimerTask = tasking::enqueue(
            LPC_GARBAGE_COLLECT_LOGS_AND_REPLICAS,
            this,
            &replica_stub::on_gc,
            0,
            random32(0, _options.GcIntervalMs),
            _options.GcIntervalMs
            );
    }

    multi_partition_decrees initMaxDecrees; // for log truncate
    for (auto it = replicas.begin(); it != replicas.end(); it++)
    {
        initMaxDecrees[it->second->get_gpid()] = it->second->max_prepared_decree();
    }
    err = _log->start_write_service(initMaxDecrees, _options.StalenessForCommit);
    dassert (err == ERR_SUCCESS, "");

    // attach replicas
    _replicas = replicas;

    replicas.clear();

    // start timer for configuration sync
    if (!_options.ConfigurationSyncDisabled)
    {
        _partitionConfigurationSyncTimerTask = tasking::enqueue(
            LPC_QUERY_CONFIGURATION_ALL,
            this,
            &replica_stub::query_configuration,
            0, 
            _options.ConfigurationSyncIntervalMs,
            _options.ConfigurationSyncIntervalMs
            );
    }
    
    // init livenessmonitor
    dassert (NS_Disconnected == _state, "");
    if (_options.FD_disabled == false)
    {
        _failure_detector = new replication_failure_detector(this, _options.MetaServers);
        _failure_detector->start(
            _options.FD_check_interval_seconds,
            _options.FD_beacon_interval_seconds,
            _options.FD_lease_seconds,
            _options.FD_grace_seconds
            );
        _failure_detector->register_master(_failure_detector->current_server_contact());
    }
    else
    {
        _state = NS_Connected;
    }
}

void replica_stub::replay_mutation(mutation_ptr& mu, Replicas* replicas)
{
    auto it = replicas->find(mu->data.header.gpid);
    if (it != replicas->end())
    {
        it->second->replay_mutation(mu);
    }
}

replica_ptr replica_stub::get_replica(global_partition_id gpid, bool new_when_possible, const char* app_type)
{
    zauto_lock l(_replicasLock);
    auto it = _replicas.find(gpid);
    if (it != _replicas.end())
        return it->second;
    else
    {
        if (!new_when_possible)
            return nullptr;
        else
        {
            dassert (app_type, "");
            replica* rep = replica::newr(this, app_type, gpid, _options);
            if (rep != nullptr) 
            {
                add_replica(rep);
            }
            return rep;
        }
    }
}

replica_ptr replica_stub::get_replica(int32_t app_id, int32_t partition_index)
{
    global_partition_id gpid;
    gpid.app_id = app_id;
    gpid.pidx = partition_index;
    return get_replica(gpid);
}

void replica_stub::get_primary_replica_list(uint32_t p_tableID, std::vector<global_partition_id>& p_repilcaList)
{
    zauto_lock l(_replicasLock);
    for (auto it = _replicas.begin(); it != _replicas.end(); it++)
    {
        if (it->second->status() == PS_PRIMARY 
            && (p_tableID == (uint32_t)-1 
            || it->second->get_gpid().app_id == static_cast<int>(p_tableID) ))
        {
            p_repilcaList.push_back(it->second->get_gpid());
        }
    }
}

void replica_stub::on_client_write(message_ptr& request)
{
    write_request_header hdr;
    unmarshall(request, hdr);    
    
    replica_ptr rep = get_replica(hdr.gpid);
    if (rep != nullptr)
    {
        //PerformanceCounters::Increment(PerfCounters_TotalClientWriteQps, nullptr);
        rep->on_client_write(hdr.code, request);
    }
    else
    {
        response_client_error(request, ERR_OBJECT_NOT_FOUND);
    }
}

void replica_stub::on_client_read(message_ptr& request)
{
    read_request_header req;
    unmarshall(request, req);

    replica_ptr rep = get_replica(req.gpid);
    if (rep != nullptr)
    {
        //PerformanceCounters::Increment(PerfCounters_TotalClientReadQps, nullptr);
        rep->on_client_read(req, request);
    }
    else
    {
        response_client_error(request, ERR_OBJECT_NOT_FOUND);
    }
}

void replica_stub::on_config_proposal(const configuration_update_request& proposal)
{
    if (!is_connected()) return;

    replica_ptr rep = get_replica(proposal.config.gpid, proposal.type == CT_ASSIGN_PRIMARY, proposal.config.app_type.c_str());
    if (rep == nullptr && proposal.type == CT_ASSIGN_PRIMARY)
    {
        begin_open_replica(proposal.config.app_type, proposal.config.gpid);
    }

    if (rep != nullptr)
    {
        rep->on_config_proposal((configuration_update_request&)proposal);
    }
}

void replica_stub::on_query_decree(const query_replica_decree_request& req, __out_param query_replica_decree_response& resp)
{
    replica_ptr rep = get_replica(req.gpid);
    if (rep != nullptr)
    {
        resp.err = ERR_SUCCESS;
        if (PS_POTENTIAL_SECONDARY == rep->status())
        {
            resp.last_decree = 0;
        }
        else
        {
            resp.last_decree = rep->last_committed_decree();
            // TODO: use the following to alleviate data lost
            //resp.last_decree = rep->last_prepared_decree();
        }
    }
    else
    {
        resp.err = ERR_OBJECT_NOT_FOUND;
        resp.last_decree = 0;
    }
}

void replica_stub::on_prepare(message_ptr& request)
{
    global_partition_id gpid;
    unmarshall(request, gpid);    
    replica_ptr rep = get_replica(gpid);
    if (rep != nullptr)
    {
        rep->on_prepare(request);
    }
    else
    {
        prepare_ack resp;
        resp.gpid = gpid;
        resp.err = ERR_OBJECT_NOT_FOUND;
        reply(request, resp);
    }
}

void replica_stub::on_group_check(const group_check_request& request, __out_param group_check_response& response)
{
    if (!is_connected()) return;

    replica_ptr rep = get_replica(request.config.gpid, request.config.status == PS_POTENTIAL_SECONDARY, request.app_type.c_str());
    if (rep != nullptr)
    {
        rep->on_group_check(request, response);
    }
    else 
    {
        if (request.config.status == PS_POTENTIAL_SECONDARY)
        {
            std::shared_ptr<group_check_request> req(new group_check_request);
            *req = request;

            begin_open_replica(request.app_type, request.config.gpid, req);
            response.err = ERR_SUCCESS;
            response.learner_signature = 0;
        }
        else
        {
            response.err = ERR_OBJECT_NOT_FOUND;
        }
    }
}

void replica_stub::on_learn(const learn_request& request, __out_param learn_response& response)
{
    replica_ptr rep = get_replica(request.gpid);
    if (rep != nullptr)
    {
        rep->on_learn(request, response);
    }
    else
    {
        response.err = ERR_OBJECT_NOT_FOUND;
    }
}

void replica_stub::on_learn_completion_notification(const group_check_response& report)
{
    replica_ptr rep = get_replica(report.gpid);
    if (rep != nullptr)
    {
        rep->on_learn_completion_notification(report);
    }
}

void replica_stub::on_add_learner(const group_check_request& request)
{
    replica_ptr rep = get_replica(request.config.gpid, true, request.app_type.c_str());
    if (rep != nullptr)
    {
        rep->on_add_learner(request);
    }
    else
    {
        std::shared_ptr<group_check_request> req(new group_check_request);
        *req = request;
        begin_open_replica(request.app_type, request.config.gpid, req);
    }
}

void replica_stub::on_remove(const replica_configuration& request)
{
    replica_ptr rep = get_replica(request.gpid);
    if (rep != nullptr)
    {
        rep->on_remove(request);
    }
}

void replica_stub::query_configuration()
{
    if (_state == NS_Disconnected)
    {
        return;
    }

    if (_partitionConfigurationQueryTask != nullptr)
    {
        _partitionConfigurationQueryTask->cancel(false);
    }

    message_ptr msg = message::create_request(RPC_CM_CALL, _options.meta_server_call_timeout_ms);

    meta_request_header hdr;
    hdr.rpc_tag = RPC_CM_QUERY_NODE_PARTITIONS;
    marshall(msg, hdr);

    configuration_query_by_node_request req;
    req.node = address();
    marshall(msg, req);

    _partitionConfigurationQueryTask = rpc::call_replicated(
        _failure_detector->current_server_contact(),
        _failure_detector->get_servers(),
        msg,
        this,
        std::bind(&replica_stub::on_node_query_reply, this, 
            std::placeholders::_1, 
            std::placeholders::_2, 
            std::placeholders::_3
            )
        );
}

void replica_stub::on_meta_server_connected()
{
    ddebug(
        "%s:%u: meta server connected",
        address().name.c_str(), static_cast<int>(address().port)
        );

    zauto_lock l(_replicasLock);
    if (_state == NS_Disconnected)
    {
        _state = NS_Connecting;
        query_configuration();
    }
}

void replica_stub::on_node_query_reply(int err, message_ptr& request, message_ptr& response)
{
    ddebug(
        "%s:%u: node view replied",
        address().name.c_str(), static_cast<int>(address().port)
        );

    if (response == nullptr)
    {
        zauto_lock l(_replicasLock);
        if (_state == NS_Connecting)
        {
            query_configuration();
        }
    }
    else
    {
        zauto_lock l(_replicasLock);
        if (_state == NS_Connecting)
        {
            _state = NS_Connected;
        }

        // DO NOT UPDATE STATE WHEN DISCONNECTED
        if (_state != NS_Connected)
            return;

        configuration_query_by_node_response resp;
        
        unmarshall(response, resp);        
        
        Replicas rs = _replicas;
        for (auto it = resp.partitions.begin(); it != resp.partitions.end(); it++)
        {
            rs.erase(it->gpid);
            tasking::enqueue(
                LPC_QUERY_NODE_CONFIGURATION_SCATTER,
                this,
                std::bind(&replica_stub::on_node_query_reply_scatter, this, this, *it),
                gpid_to_hash(it->gpid)
                );
        }

        // for replicas not exist on meta_servers
        for (auto it = rs.begin(); it != rs.end(); it++)
        {
            tasking::enqueue(
                LPC_QUERY_NODE_CONFIGURATION_SCATTER,
                this,
                std::bind(&replica_stub::on_node_query_reply_scatter2, this, this, it->first),
                gpid_to_hash(it->first)
                );
        }
    }
}

void replica_stub::set_meta_server_connected_for_test(const configuration_query_by_node_response& resp)
{
    zauto_lock l(_replicasLock);
    dassert (_state != NS_Connected, "");
    _state = NS_Connected;

    for (auto it = resp.partitions.begin(); it != resp.partitions.end(); it++)
    {
        tasking::enqueue(
            LPC_QUERY_NODE_CONFIGURATION_SCATTER,
            this,
            std::bind(&replica_stub::on_node_query_reply_scatter, this, this, *it),
            gpid_to_hash(it->gpid)
            );
    }
}

// this_ is used to hold a ref to replica_stub so we don't need to cancel the task on replica_stub::close
void replica_stub::on_node_query_reply_scatter(replica_stub_ptr this_, const partition_configuration& config)
{
    replica_ptr replica = get_replica(config.gpid);
    if (replica != nullptr)
    {
        replica->on_config_sync(config);
    }
    else
    {
        remove_replica_on_meta_server(config);
    }
}

void replica_stub::on_node_query_reply_scatter2(replica_stub_ptr this_, global_partition_id gpid)
{
    replica_ptr replica = get_replica(gpid);
    if (replica != nullptr)
    {
        ddebug(
            "%u.%u @ %s:%u: replica not exists on meta server, removed",
            gpid.app_id, gpid.pidx,
            address().name.c_str(), static_cast<int>(address().port)
            );
        replica->update_local_configuration_with_no_ballot_change(PS_ERROR);
    }
}

void replica_stub::remove_replica_on_meta_server(const partition_configuration& config)
{
    message_ptr msg = message::create_request(RPC_CM_CALL, _options.meta_server_call_timeout_ms);
    meta_request_header hdr;
    hdr.rpc_tag = RPC_CM_UPDATE_PARTITION_CONFIGURATION;
    marshall(msg, hdr);

    std::shared_ptr<configuration_update_request> request(new configuration_update_request);
    request->config = config;
    request->config.ballot++;        
    request->node = address();
    request->type = CT_DOWNGRADE_TO_INACTIVE;

    if (address() == config.primary)
    {
        request->config.primary = dsn::end_point::INVALID;        
    }
    else if (ReplicaHelper::RemoveNode(address(), request->config.secondaries))
    {
    }
    else
    {
        return;
    }

    marshall(msg, *request);

    rpc::call_replicated(
        _failure_detector->current_server_contact(),
        _failure_detector->get_servers(),
        msg,
        nullptr,
        nullptr
        );
}

void replica_stub::on_meta_server_disconnected()
{
    ddebug(
        "%s:%u: meta server disconnected",
        address().name.c_str(), static_cast<int>(address().port)
        );
    zauto_lock l(_replicasLock);
    if (NS_Disconnected == _state)
        return;

    _state = NS_Disconnected;

    for (auto it = _replicas.begin(); it != _replicas.end(); it++)
    {
        tasking::enqueue(
            LPC_CM_DISCONNECTED_SCATTER,
            this,
            std::bind(&replica_stub::on_meta_server_disconnected_scatter, this, this, it->first),
            gpid_to_hash(it->first)
            );
    }
}

// this_ is used to hold a ref to replica_stub so we don't need to cancel the task on replica_stub::close
void replica_stub::on_meta_server_disconnected_scatter(replica_stub_ptr this_, global_partition_id gpid)
{
    {
        zauto_lock l(_replicasLock);
        if (_state != NS_Disconnected)
            return;
    }

    replica_ptr replica = get_replica(gpid);
    if (replica != nullptr)
    {
        replica->on_meta_server_disconnected();
    }
}

void replica_stub::response_client_error(message_ptr& request, int error)
{
    message_ptr resp = request->create_response();
    resp->writer().write(error);
    rpc::reply(resp);
}

void replica_stub::init_gc_for_test()
{
    dassert (_options.GcDisabled, "");

    _gcTimerTask = tasking::enqueue(
        LPC_GARBAGE_COLLECT_LOGS_AND_REPLICAS,
        this,
        &replica_stub::on_gc,
        0,
        _options.GcIntervalMs
        );
}

void replica_stub::on_gc()
{
    Replicas rs;
    {
        zauto_lock l(_replicasLock);
        rs = _replicas;
    }

    // gc log
    multi_partition_decrees durable_decrees;
    for (auto it = rs.begin(); it != rs.end(); it++)
    {
        durable_decrees[it->first] = it->second->last_durable_decree();
    }
    _log->garbage_collection(durable_decrees);
    
    // gc on-disk replicas
    boost::filesystem::directory_iterator endtr;
    for (boost::filesystem::directory_iterator it(dir());
        it != endtr;
        ++it)
    {
        auto name = it->path().filename().string();
        if (name.length() > strlen(".err") && name.substr(name.length() - strlen(".err")) == ".err")
        {
            std::time_t mt = boost::filesystem::last_write_time(it->path());
            if (mt > time(0) + _options.GcDiskErrorReplicaIntervalSeconds)
            {
                boost::filesystem::remove_all(_dir + "/" + name);
            }
        }
    }
}

task_ptr replica_stub::begin_open_replica(const std::string& app_type, global_partition_id gpid, std::shared_ptr<group_check_request> req)
{
    _replicasLock.lock();
    if (_replicas.find(gpid) != _replicas.end())
    {
        _replicasLock.unlock();
        return nullptr;
    }        

    auto it = _openingReplicas.find(gpid);
    if (it != _openingReplicas.end())
    {
        _replicasLock.unlock();
        return nullptr;
    }
    else 
    {
        auto it2 = _closingReplicas.find(gpid);
        if (it2 != _closingReplicas.end())
        {
            if (it2->second.second->status() == PS_INACTIVE 
                && it2->second.first->cancel(false))
            {
                replica_ptr r = it2->second.second;
                _closingReplicas.erase(it2);
                add_replica(r);

                // unlock here to avoid dead lock
                _replicasLock.unlock();

                ddebug( "open replica which is to be closed '%s.%u.%u'", app_type.c_str(), gpid.app_id, gpid.pidx);

                if (req != nullptr)
                {
                    on_add_learner(*req);
                }
                return nullptr;
            }
            else 
            {
                _replicasLock.unlock();
                dwarn( "open replica '%s.%u.%u' failed coz replica is under closing", 
                    app_type.c_str(), gpid.app_id, gpid.pidx);                
                return nullptr;
            }
        }
        else 
        {
            auto task = tasking::enqueue(LPC_OPEN_REPLICA, this, std::bind(&replica_stub::open_replica, this, app_type, gpid, req));
            _openingReplicas[gpid] = task;
            _replicasLock.unlock();
            return task;
        }
    }
}

void replica_stub::open_replica(const std::string app_type, global_partition_id gpid, std::shared_ptr<group_check_request> req)
{
    char buffer[256];
    sprintf(buffer, "%u.%u.%s", gpid.app_id, gpid.pidx, app_type.c_str());

    std::string dr = dir() + "/" + buffer;

    dwarn("open replica '%s'", dr.c_str());

    replica_ptr rep = replica::load(this, dr.c_str(), _options, true);
    if (rep == nullptr) rep = replica::newr(this, app_type.c_str(), gpid, _options);
    dassert (rep != nullptr, "");
        
    {
        zauto_lock l(_replicasLock);
        auto it = _replicas.find(gpid);
        dassert (it == _replicas.end(), "");
        add_replica(rep);
        _openingReplicas.erase(gpid);
    }

    if (nullptr != req)
    {
        rpc::call_one_way_typed(address(), RPC_LEARN_ADD_LEARNER, *req, gpid_to_hash(req->config.gpid));
    }
}

task_ptr replica_stub::begin_close_replica(replica_ptr r)
{
    zauto_lock l(_replicasLock);

    // initialization is still ongoing
    if (nullptr == _failure_detector)
        return nullptr;

    if (remove_replica(r))
    {
        auto task = tasking::enqueue(LPC_CLOSE_REPLICA, this, 
            std::bind(&replica_stub::close_replica, this, r), 
            0, 
            r->status() == PS_ERROR ? 0 : _options.GcMemoryReplicaIntervalMs
            );
        _closingReplicas[r->get_gpid()] = std::make_pair(task, r);
        return task;
    }
    else
    {
        return nullptr;
    }
}

void replica_stub::close_replica(replica_ptr r)
{
    dwarn( "close replica '%s'", r->dir().c_str());

    r->close();

    {
        zauto_lock l(_replicasLock);
        _closingReplicas.erase(r->get_gpid());
    }
}

void replica_stub::add_replica(replica_ptr r)
{
    zauto_lock l(_replicasLock);
    _replicas[r->get_gpid()] = r;
}

bool replica_stub::remove_replica(replica_ptr r)
{
    zauto_lock l(_replicasLock);
    if (_replicas.erase(r->get_gpid()) > 0)
    {
        return true;
    }
    else
        return false;
}

void replica_stub::notify_replica_state_update(const replica_configuration& config, bool isClosing)
{
    if (nullptr != _replicaStateSubscriber)
    {
        if (_isLongSubscriber)
        {
            tasking::enqueue(LPC_REPLICA_STATE_CHANGE_NOTIFICATION, this, std::bind(_replicaStateSubscriber, address(), config, isClosing));
        }
        else
        {
            _replicaStateSubscriber(address(), config, isClosing);
        }
    }
}

void replica_stub::open_service()
{
    register_rpc_handler(RPC_REPLICATION_CLIENT_WRITE, "write", &replica_stub::on_client_write);
    register_rpc_handler(RPC_REPLICATION_CLIENT_READ, "read", &replica_stub::on_client_read);

    register_rpc_handler(RPC_CONFIG_PROPOSAL, "ProposeConfig", &replica_stub::on_config_proposal);

    register_rpc_handler(RPC_PREPARE, "prepare", &replica_stub::on_prepare);
    register_rpc_handler(RPC_LEARN, "Learn", &replica_stub::on_learn);
    register_rpc_handler(RPC_LEARN_COMPLETITION_NOTIFY, "LearnNotify", &replica_stub::on_learn_completion_notification);
    register_rpc_handler(RPC_LEARN_ADD_LEARNER, "LearnAdd", &replica_stub::on_add_learner);
    register_rpc_handler(RPC_REMOVE_REPLICA, "remove", &replica_stub::on_remove);
    register_rpc_handler(RPC_GROUP_CHECK, "GroupCheck", &replica_stub::on_group_check);
    register_rpc_handler(RPC_QUERY_PN_DECREE, "QueryDecree", &replica_stub::on_query_decree);
}

void replica_stub::close()
{
    if (_partitionConfigurationSyncTimerTask != nullptr)
    {
        _partitionConfigurationSyncTimerTask->cancel(true);
        _partitionConfigurationSyncTimerTask = nullptr;
    }

    if (_partitionConfigurationQueryTask != nullptr)
    {
        _partitionConfigurationQueryTask->cancel(true);
        _partitionConfigurationQueryTask = nullptr;
    }
    _state = NS_Disconnected;
    
    if (_gcTimerTask != nullptr)
    {
        _gcTimerTask->cancel(true);
        _gcTimerTask = nullptr;
    }
    
    {
        zauto_lock l(_replicasLock);    
        while (_closingReplicas.empty() == false)
        {
            auto task = _closingReplicas.begin()->second.first;
            _replicasLock.unlock();

            task->wait();

            _replicasLock.lock();
            _closingReplicas.erase(_closingReplicas.begin());
        }

        while (_openingReplicas.empty() == false)
        {
            auto task = _openingReplicas.begin()->second;
            _replicasLock.unlock();

            task->cancel(true);

            _replicasLock.lock();
            _openingReplicas.erase(_openingReplicas.begin());
        }

        while (_replicas.empty() == false)
        {
            _replicas.begin()->second->close();
            _replicas.erase(_replicas.begin());
        }
    }
        
    if (_failure_detector != nullptr)
    {
        _failure_detector->stop();
        delete _failure_detector;
        _failure_detector = nullptr;
    }

    if (_log != nullptr)
    {
        _log->close();
        delete _log;
        _log = nullptr;
    }
}

}} // namespace

