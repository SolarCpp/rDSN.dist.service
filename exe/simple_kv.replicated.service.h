/**
* Autogenerated by Thrift Compiler (@PACKAGE_VERSION@)
*
* DO NOT EDIT UNLESS YOU ARE SURE THAT YOU KNOW WHAT YOU ARE DOING
*  @generated
*/
#ifndef simple_kv_rDSN2_H
#define simple_kv_rDSN2_H

#include "simple_kv_types.h"
#include <dsn/serverlet.h>
#include <iostream>
#include <dsn/dist/replication.h>
#include "simple_kv.replicated.client.h"

namespace dsn {
    namespace replication {
        namespace application {

            // server
            template <typename T> // where T : simple_kv_service<T>
            class simple_kv_service
                : public ::dsn::replication::replication_app_base
            {
            public:
                simple_kv_service(replica* replica, const replication_app_config* config)
                    : replication_app_base(replica, config)
                {
                    open_service();
                }
                virtual ~simple_kv_service() 
                {
                    close_service();
                }

            protected:
                // all service handlers to be implemented further
                // RPC_SIMPLE_KV_READ
                virtual void on_read(const std::string& key, ::dsn::service::rpc_replier<std::string>& reply) = 0;
                // RPC_SIMPLE_KV_WRITE
                virtual void on_write(const kv_pair& pr, ::dsn::service::rpc_replier<int32_t>& reply) = 0;
                // RPC_SIMPLE_KV_APPEND
                virtual void on_append(const kv_pair& pr, ::dsn::service::rpc_replier<int32_t>& reply) = 0;

            public:
                void open_service()
                {
                    this->register_async_rpc_handler(RPC_SIMPLE_KV_READ, "read", &T::on_read);
                    this->register_async_rpc_handler(RPC_SIMPLE_KV_WRITE, "write", &T::on_write);
                    this->register_async_rpc_handler(RPC_SIMPLE_KV_APPEND, "append", &T::on_append);
                }

                void close_service()
                {
                    this->unregister_rpc_handler(RPC_SIMPLE_KV_READ);
                    this->unregister_rpc_handler(RPC_SIMPLE_KV_WRITE);
                    this->unregister_rpc_handler(RPC_SIMPLE_KV_APPEND);
                }
            };

        }
    }
} // namespace


#endif