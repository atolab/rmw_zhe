// Copyright 2018 ADLINK Technology Limited.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string>
#include <utility>
#include <vector>
#include <thread>

#include "rcutils/allocator.h"
#include "rcutils/filesystem.h"
#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"
#include "rcutils/types.h"

#include "rmw/allocators.h"
#include "rmw/convert_rcutils_ret_to_rmw_ret.h"
#include "rmw/error_handling.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"
#include "rmw/sanity_checks.h"

#include "rmw/impl/cpp/macros.hpp"

#include "MurmurHash3.h"
#include "namespace_prefix.hpp"
#include "fastcdr/FastBuffer.h"

#include "rmw_zhe_cpp/MessageTypeSupport.hpp"
#include "rmw_zhe_cpp/ServiceTypeSupport.hpp"

#include "zhe.h"
#include "platform-udp.h"

#define RET_ERR_X(msg, code) do { RMW_SET_ERROR_MSG(msg); code; } while(0)
#define RET_NULL_X(var, code) do { if (!var) RET_ERR_X(#var " is null", code); } while(0)
#define RET_ALLOC_X(var, code) do { if (!var) RET_ERR_X("failed to allocate " #var, code); } while(0)
#define RET_WRONG_IMPLID_X(var, code) do {                              \
        RET_NULL_X(var, code);                                          \
        if ((var)->implementation_identifier != adlink_zhe_identifier) { \
            RET_ERR_X(#var " not from this implementation", code);      \
        }                                                               \
    } while(0)
#define RET_NULL_OR_EMPTYSTR_X(var, code) do {                  \
          if(!var || strlen(var) == 0) {                        \
              RET_ERR_X(#var " is null or empty string", code); \
          }                                                     \
      } while(0)
#define RET_ERR(msg) RET_ERR_X(msg, return RMW_RET_ERROR)
#define RET_NULL(var) RET_NULL_X(var, return RMW_RET_ERROR)
#define RET_ALLOC(var) RET_ALLOC_X(var, return RMW_RET_ERROR)
#define RET_WRONG_IMPLID(var) RET_WRONG_IMPLID_X(var, return RMW_RET_ERROR)
#define RET_NULL_OR_EMPTYSTR(var) RET_NULL_OR_EMPTYSTR_X(var, return RMW_RET_ERROR)

const char *const adlink_zhe_identifier = "rmw_zhe_cpp";

struct condition {
    std::mutex internalMutex_;
    std::atomic_bool hasTriggered_;
    std::mutex *conditionMutex_;
    std::condition_variable *conditionVariable_;
    condition() : hasTriggered_(false), conditionMutex_(nullptr), conditionVariable_(nullptr) { }
};

static void condition_set_trigger_locked(struct condition *cond, bool value);

struct ZheTypeSupport {
    void *type_support_;
    const char *typesupport_identifier_;
};

/* this had better be compatible with the "guid" field in the rmw_request_id_t and the data field in rmw_gid_t */
typedef std::array<uint8_t,16> zhe_guid_t;
typedef std::array<uint8_t,RMW_GID_STORAGE_SIZE> zhe_gid_t;

struct ZhePublisher {
    zhe_pubidx_t pubh;
    zhe_gid_t publisher_gid;
    zhe_guid_t publisher_guid;
    ZheTypeSupport ts;
};

struct histelem {
    struct histelem *newer; /* circular */
    void *data;
    size_t size;
};

struct hist {
    size_t depth;
    size_t count;
    struct histelem *newest;
};

struct ZheSubscription {
    typedef rmw_subscription_t rmw_type;
    typedef rmw_subscriptions_t rmw_set_type;
    zhe_subidx_t subh;
    ZheTypeSupport ts;
    struct hist hist;
    struct condition cond; /* cond.internalMutex_ doubles as a lock for ZheSubscription */
};

struct ZheCS {
    ZhePublisher *pub;
    ZheSubscription *sub;
};

struct ZheClient {
    typedef rmw_client_t rmw_type;
    typedef rmw_clients_t rmw_set_type;
    ZheCS client;
};

struct ZheService {
    typedef rmw_service_t rmw_type;
    typedef rmw_services_t rmw_set_type;
    ZheCS service;
};

struct ZheNode {
    rmw_guard_condition_t *graph_guard_condition;
};

struct ZheGuardCondition {
    struct condition cond;
};

struct Zhe {
    volatile std::atomic_int count;
    zhe_platform *platform;
    std::mutex lock;
    std::thread thread;
    zhe_address_t scoutaddr;
    unsigned char ownid[8];
    std::atomic_uint next_request_id;
};
static Zhe gzhe;

static ZheSubscription **begin(rmw_subscriptions_t& s) {
    return (ZheSubscription **)(&s.subscribers[0]);
}
static ZheSubscription **end(rmw_subscriptions_t& s) {
    return (ZheSubscription **)(&s.subscribers[s.subscriber_count]);
}
static ZheClient **begin(rmw_clients_t& s) {
    return (ZheClient **)(&s.clients[0]);
}
static ZheClient **end(rmw_clients_t& s) {
    return (ZheClient **)(&s.clients[s.client_count]);
}
static ZheService **begin(rmw_services_t& s) {
    return (ZheService **)(&s.services[0]);
}
static ZheService **end(rmw_services_t& s) {
    return (ZheService **)(&s.services[s.service_count]);
}
static ZheGuardCondition **begin(rmw_guard_conditions_t& s) {
    return (ZheGuardCondition **)(&s.guard_conditions[0]);
}
static ZheGuardCondition **end(rmw_guard_conditions_t& s) {
    return (ZheGuardCondition **)(&s.guard_conditions[s.guard_condition_count]);
}
static const ZheSubscription * const *begin(const rmw_subscriptions_t& s) {
    return (const ZheSubscription **)(&s.subscribers[0]);
}
static const ZheSubscription * const *end(const rmw_subscriptions_t& s) {
    return (const ZheSubscription **)(&s.subscribers[s.subscriber_count]);
}
static const ZheClient * const *begin(const rmw_clients_t& s) {
    return (const ZheClient * const *)(&s.clients[0]);
}
static const ZheClient * const *end(const rmw_clients_t& s) {
    return (const ZheClient * const *)(&s.clients[s.client_count]);
}
static const ZheService * const *begin(const rmw_services_t& s) {
    return (const ZheService * const *)(&s.services[0]);
}
static const ZheService * const *end(const rmw_services_t& s) {
    return (const ZheService * const *)(&s.services[s.service_count]);
}
static const ZheGuardCondition * const *begin(const rmw_guard_conditions_t& s) {
    return (const ZheGuardCondition * const *)(&s.guard_conditions[0]);
}
static const ZheGuardCondition * const *end(const rmw_guard_conditions_t& s) {
    return (const ZheGuardCondition * const *)(&s.guard_conditions[s.guard_condition_count]);
}

struct condition *get_condition(ZheSubscription *x) { return &x->cond; }
struct condition *get_condition(ZheClient *x) { return &x->client.sub->cond; }
struct condition *get_condition(ZheService *x) { return &x->service.sub->cond; }
struct condition *get_condition(ZheGuardCondition *x) { return &x->cond; }
template <typename T> const condition *get_condition(const T *x) { return get_condition(const_cast<T *>(x)); }

template<typename T> void condition_set_trigger(T *x, bool value) {
    condition_set_trigger(get_condition(x), value);
}
template<typename T> bool condition_read(T *x) {
    return condition_read(get_condition(x));
}
template<typename T> bool condition_take(T *x) {
    return condition_take(get_condition(x));
}
template<typename T> void condition_attach(T *x, std::mutex *conditionMutex, std::condition_variable *conditionVariable) {
    condition_attach(get_condition(x), conditionMutex, conditionVariable);
}
template<typename T> void condition_detach(T *x) {
    condition_detach(get_condition(x));
}

typedef struct zhe_request_header {
    zhe_guid_t guid;
    int64_t seq;
} zhe_request_header_t;

typedef struct zhe_sample_header {
    zhe_gid_t gid;
} zhe_sample_header_t;

static void zhe_thread()
{
    while(gzhe.count > 0) {
        zhe_time_t tnow = zhe_platform_time();
        zhe_platform_waitinfo_t waitinfo;
    
        {
            std::lock_guard<std::mutex> guard(gzhe.lock);
            zhe_housekeeping(tnow);
            zhe_platform_wait_prep(&waitinfo, gzhe.platform);
        }
    
        if (zhe_platform_wait_block(&waitinfo, 10)) {
            std::lock_guard<std::mutex> guard(gzhe.lock);
            zhe_recvbuf_t inbuf;
            zhe_address_t insrc;
            int recvret;
            tnow = zhe_platform_time();
            if ((recvret = zhe_platform_recv(gzhe.platform, &inbuf, &insrc)) > 0) {
                int n = zhe_input(inbuf.buf, (size_t) recvret, &insrc, tnow);
                zhe_platform_advance(gzhe.platform, &insrc, n);
            }
        }
    }
}

static zhe_paysize_t getrandomid(unsigned char *ownid, size_t ownidsize)
{
    FILE *fp;
    if ((fp = fopen("/dev/urandom", "rb")) == NULL) {
        perror("can't open /dev/urandom\n");
        exit(1);
    }
    if (fread(ownid, ownidsize, 1, fp) != 1) {
        fprintf(stderr, "can't read %zu random bytes from /dev/urandom\n", ownidsize);
        fclose(fp);
        exit(1);
    }
    fclose(fp);
    return (zhe_paysize_t)ownidsize;
}

static bool zhe_add_node()
{
    std::lock_guard<std::mutex> guard(gzhe.lock);
    if (gzhe.count++ == 0) {
        struct zhe_config cfg;
        getrandomid(gzhe.ownid, sizeof(gzhe.ownid));
        memset(&cfg, 0, sizeof(cfg));
        cfg.id = gzhe.ownid;
        cfg.idlen = sizeof(gzhe.ownid);
        cfg.scoutaddr = &gzhe.scoutaddr;
        if (zhe_init(&cfg, gzhe.platform, zhe_platform_time()) < 0) {
            RMW_SET_ERROR_MSG("failed to initialize Zhe");
            return false;
        }
        gzhe.next_request_id = 1;
        zhe_start(zhe_platform_time());
        gzhe.thread = std::thread(zhe_thread);
    }
    return true;
}

static void zhe_remove_node()
{
    //FIXME: race condition if add_node may be called on a different and this was the last node
    //(trivially fixed, just can't be bothered right now)
    bool join;
    {
        std::lock_guard<std::mutex> guard(gzhe.lock);
        join = (--gzhe.count == 0);
    }
    if (join) {
        gzhe.thread.join();
    }
}

static void make_gid_from_pubidx(zhe_gid_t *dst, zhe_pubidx_t pubh)
{
    static_assert(sizeof(gzhe.ownid) + sizeof(pubh) <= RMW_GID_STORAGE_SIZE, "RMW_GID_STORAGE_SIZE insufficient to store the rmw_zhe_cpp GID implementation.");
    memset(dst->data(), 0, RMW_GID_STORAGE_SIZE);
    memcpy(dst->data(), gzhe.ownid, sizeof(gzhe.ownid));
    memcpy(dst->data() + sizeof(gzhe.ownid), &pubh, sizeof(pubh));
}

static void make_guid_from_gid(zhe_guid_t *dst, const zhe_gid_t *src)
{
    const size_t copysz = sizeof(*dst) < sizeof(*src) ? sizeof(*dst) : sizeof(*src);
    memcpy(dst->data(), src->data(), copysz);
    memset(dst->data() + copysz, 0, sizeof(*dst) - copysz);
}

using MessageTypeSupport_c = rmw_zhe_cpp::MessageTypeSupport<rosidl_typesupport_introspection_c__MessageMembers>;
using MessageTypeSupport_cpp = rmw_zhe_cpp::MessageTypeSupport<rosidl_typesupport_introspection_cpp::MessageMembers>;
using RequestTypeSupport_c = rmw_zhe_cpp::RequestTypeSupport<rosidl_typesupport_introspection_c__ServiceMembers, rosidl_typesupport_introspection_c__MessageMembers>;
using RequestTypeSupport_cpp = rmw_zhe_cpp::RequestTypeSupport<rosidl_typesupport_introspection_cpp::ServiceMembers, rosidl_typesupport_introspection_cpp::MessageMembers>;
using ResponseTypeSupport_c = rmw_zhe_cpp::ResponseTypeSupport<rosidl_typesupport_introspection_c__ServiceMembers, rosidl_typesupport_introspection_c__MessageMembers>;
using ResponseTypeSupport_cpp = rmw_zhe_cpp::ResponseTypeSupport<rosidl_typesupport_introspection_cpp::ServiceMembers, rosidl_typesupport_introspection_cpp::MessageMembers>;

static bool using_introspection_c_typesupport(const char *typesupport_identifier)
{
    return typesupport_identifier == rosidl_typesupport_introspection_c__identifier;
}

static bool using_introspection_cpp_typesupport(const char *typesupport_identifier)
{
    return typesupport_identifier == rosidl_typesupport_introspection_cpp::typesupport_identifier;
}

static void *_create_message_type_support(const void *untyped_members, const char *typesupport_identifier)
{
    if (using_introspection_c_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(
                untyped_members);
        return new MessageTypeSupport_c(members);
    } else if (using_introspection_cpp_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(
                untyped_members);
        return new MessageTypeSupport_cpp(members);
    }
    RMW_SET_ERROR_MSG("Unknown typesupport identifier");
    return nullptr;
}

static void *_create_request_type_support(const void *untyped_members, const char *typesupport_identifier)
{
    if (using_introspection_c_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *>(untyped_members);
        return new RequestTypeSupport_c(members);
    } else if (using_introspection_cpp_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers *>(untyped_members);
        return new RequestTypeSupport_cpp(members);
    }
    RMW_SET_ERROR_MSG("Unknown typesupport identifier");
    return nullptr;
}

static void *_create_response_type_support(const void *untyped_members, const char *typesupport_identifier)
{
    if (using_introspection_c_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *>(untyped_members);
        return new ResponseTypeSupport_c(members);
    } else if (using_introspection_cpp_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers *>(untyped_members);
        return new ResponseTypeSupport_cpp(members);
    }
    RMW_SET_ERROR_MSG("Unknown typesupport identifier");
    return nullptr;
}

template<typename ServiceType> const void *get_request_ptr(const void *untyped_service_members)
{
    auto service_members = static_cast<const ServiceType *>(untyped_service_members);
    RET_NULL_X(service_members, return nullptr);
    return service_members->request_members_;
}

template<typename ServiceType> const void *get_response_ptr(const void *untyped_service_members)
{
    auto service_members = static_cast<const ServiceType *>(untyped_service_members);
    RET_NULL_X(service_members, return nullptr);
    return service_members->response_members_;
}

static bool sermsg(const void *ros_message, eprosima::fastcdr::Cdr& ser, std::function<void(eprosima::fastcdr::Cdr&)> prefix, const ZheTypeSupport& ts)
{
    if (using_introspection_c_typesupport(ts.typesupport_identifier_)) {
        auto typed_typesupport = static_cast<MessageTypeSupport_c *>(ts.type_support_);
        return typed_typesupport->serializeROSmessage(ros_message, ser, prefix);
    } else if (using_introspection_cpp_typesupport(ts.typesupport_identifier_)) {
        auto typed_typesupport = static_cast<MessageTypeSupport_cpp *>(ts.type_support_);
        return typed_typesupport->serializeROSmessage(ros_message, ser, prefix);
    }
    RMW_SET_ERROR_MSG("Unknown typesupport identifier");
    return false;
}

static bool desermsg(eprosima::fastcdr::Cdr& deser, void *ros_message, std::function<void(eprosima::fastcdr::Cdr&)> prefix, const ZheTypeSupport& ts)
{
  if (using_introspection_c_typesupport(ts.typesupport_identifier_)) {
    auto typed_typesupport = static_cast<MessageTypeSupport_c *>(ts.type_support_);
    return typed_typesupport->deserializeROSmessage(deser, ros_message, prefix);
  } else if (using_introspection_cpp_typesupport(ts.typesupport_identifier_)) {
    auto typed_typesupport = static_cast<MessageTypeSupport_cpp *>(ts.type_support_);
    return typed_typesupport->deserializeROSmessage(deser, ros_message, prefix);
  }
  RMW_SET_ERROR_MSG("Unknown typesupport identifier");
  return false;
}

extern "C"
{
#pragma GCC visibility push(default)

    const char *rmw_get_implementation_identifier()
    {
        return adlink_zhe_identifier;
    }

    rmw_ret_t rmw_init()
    {
        extern unsigned zhe_trace_cats;
        zhe_trace_cats = (getenv("ZHE_TRACE") ? atoi(getenv("ZHE_TRACE")) : 0);
        
        gzhe.platform = static_cast<zhe_platform *>(zhe_platform_new(7447, 0));
        if (gzhe.platform == nullptr) {
            RMW_SET_ERROR_MSG("failed in zhe_platform_new");
            return RMW_RET_ERROR;
        }
        zhe_platform_string2addr(gzhe.platform, &gzhe.scoutaddr, "239.255.0.1");
        if (!zhe_platform_join(gzhe.platform, &gzhe.scoutaddr)) {
            RMW_SET_ERROR_MSG("failed in zhe_platform_join");
            return RMW_RET_ERROR;
        }
        return RMW_RET_OK;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    NODES                                                          ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////
  
    rmw_node_t *rmw_create_node(const char *name, const char *namespace_, size_t domain_id, const rmw_node_security_options_t *security_options)
    {
        RET_NULL_X(name, return nullptr);
        RET_NULL_X(namespace_, return nullptr);
        (void)domain_id;
        (void)security_options;

        auto *node_impl = new ZheNode();
        rmw_node_t *node_handle = nullptr;
        RET_ALLOC_X(node_impl, goto fail_node_impl);
        rmw_guard_condition_t *graph_guard_condition;
        if (!(graph_guard_condition = rmw_create_guard_condition())) {
            goto fail_ggc;
        }
        node_impl->graph_guard_condition = graph_guard_condition;

        node_handle = rmw_node_allocate();
        RET_ALLOC_X(node_handle, goto fail_node_handle);
        node_handle->implementation_identifier = adlink_zhe_identifier;
        node_handle->data = node_impl;
    
        node_handle->name = static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(name) + 1));
        RET_ALLOC_X(node_handle->name, goto fail_node_handle_name);
        memcpy(const_cast<char *>(node_handle->name), name, strlen(name) + 1);
    
        node_handle->namespace_ = static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(namespace_) + 1));
        RET_ALLOC_X(node_handle->namespace_, goto fail_node_handle_namespace);
        memcpy(const_cast<char *>(node_handle->namespace_), namespace_, strlen(namespace_) + 1);
    
        if (!zhe_add_node()) {
            goto fail_add_node;
        }
        return node_handle;
    fail_add_node:
        rmw_free(const_cast<char *>(node_handle->namespace_));
    fail_node_handle_namespace:
        rmw_free(const_cast<char *>(node_handle->name));
    fail_node_handle_name:
        rmw_node_free(node_handle);
    fail_node_handle:
        if (RMW_RET_OK != rmw_destroy_guard_condition(graph_guard_condition)) {
            RCUTILS_LOG_ERROR_NAMED("rmw_zhe_cpp", "failed to destroy guard condition during error handling")
        }
    fail_ggc:
        delete node_impl;
    fail_node_impl:
        return nullptr;
    }

    rmw_ret_t rmw_destroy_node(rmw_node_t *node)
    {
        rmw_ret_t result_ret = RMW_RET_OK;
        RET_WRONG_IMPLID(node);
        auto node_impl = static_cast<ZheNode *>(node->data);
        RET_NULL(node_impl);
        zhe_remove_node();
        rmw_free(const_cast<char *>(node->name));
        rmw_free(const_cast<char *>(node->namespace_));
        rmw_node_free(node);
        if (RMW_RET_OK != rmw_destroy_guard_condition(node_impl->graph_guard_condition)) {
            RMW_SET_ERROR_MSG("failed to destroy graph guard condition");
            result_ret = RMW_RET_ERROR;
        }
        delete node_impl;
        return result_ret;
    }

    const rmw_guard_condition_t *rmw_node_get_graph_guard_condition(const rmw_node_t *node)
    {
        RET_WRONG_IMPLID_X(node, return nullptr);
        auto node_impl = static_cast<ZheNode *>(node->data);
        RET_NULL_X(node_impl, return nullptr);
        return node_impl->graph_guard_condition;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    PUBLICATIONS                                                   ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////
  
    static rmw_ret_t rmw_write_ser(zhe_pubidx_t pubh, eprosima::fastcdr::Cdr& ser)
    {
        const size_t sz = ser.getSerializedDataLength();
        const void *raw = static_cast<void *>(ser.getBufferPointer());
        std::lock_guard<std::mutex> lock(gzhe.lock);
        if (zhe_write(pubh, raw, sz, zhe_platform_time())) {
            return RMW_RET_OK;
        } else {
            /* FIXME: what is the expected behavior when the transmit window is full? */
            RMW_SET_ERROR_MSG("cannot publish data");
            return RMW_RET_ERROR;
        }
    }
    
    rmw_ret_t rmw_publish(const rmw_publisher_t *publisher, const void *ros_message)
    {
        RET_WRONG_IMPLID(publisher);
        RET_NULL(ros_message);
        auto pub = static_cast<ZhePublisher *>(publisher->data);
        assert(pub);
        eprosima::fastcdr::FastBuffer buffer;
        eprosima::fastcdr::Cdr ser(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
        zhe_sample_header_t header;
        header.gid = pub->publisher_gid;
        if (sermsg(ros_message, ser, [&header](eprosima::fastcdr::Cdr& ser) { ser << header.gid; }, pub->ts)) {
            return rmw_write_ser(pub->pubh, ser);
        } else {
            RMW_SET_ERROR_MSG("cannot serialize data");
            return RMW_RET_ERROR;
        }
    }

    static const rosidl_message_type_support_t *get_typesupport(const rosidl_message_type_support_t *type_supports)
    {
        const rosidl_message_type_support_t *ts;
        if ((ts = get_message_typesupport_handle(type_supports, rosidl_typesupport_introspection_c__identifier)) != nullptr) {
            return ts;
        } else if ((ts = get_message_typesupport_handle(type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr) {
            return ts;
        } else {RMW_SET_ERROR_MSG("type support not from this implementation");
            return nullptr;
        }
    }

    static std::string make_fqtopic(const char *prefix, const char *topic_name, const char *suffix, bool avoid_ros_namespace_conventions)
    {
        if (avoid_ros_namespace_conventions) {
            return std::string(topic_name) + "__" + std::string(suffix);
        } else {
            return std::string(prefix) + "/" + make_fqtopic(prefix, topic_name, suffix, true);
        }
    }

    static std::string make_fqtopic(const char *prefix, const char *topic_name, const char *suffix, const rmw_qos_profile_t *qos_policies)
    {
        return make_fqtopic(prefix, topic_name, suffix, qos_policies->avoid_ros_namespace_conventions);
    }

    static zhe_rid_t make_rid(const std::string& topic)
    {
        uint32_t hash;
        MurmurHash3_x86_32(static_cast<const void *>(topic.c_str()), topic.length(), 0, &hash);
        return static_cast<zhe_rid_t>(hash % ZHE_MAX_RID);
    }
    
    static ZhePublisher *create_zhe_publisher(const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies)
    {
        RET_WRONG_IMPLID_X(node, return nullptr);
        RET_NULL_OR_EMPTYSTR_X(topic_name, return nullptr);
        RET_NULL_X(qos_policies, return nullptr);
        auto node_impl = static_cast<ZheNode *>(node->data);
        RET_NULL_X(node_impl, return nullptr);
        const rosidl_message_type_support_t *type_support = get_typesupport(type_supports);
        RET_NULL_X(type_support, return nullptr);
        ZhePublisher *pub = new ZhePublisher();
        zhe_rid_t rid;

        pub->ts.typesupport_identifier_ = type_support->typesupport_identifier;
        pub->ts.type_support_ = _create_message_type_support(type_support->data, pub->ts.typesupport_identifier_);
        std::string topic = make_fqtopic(ros_topic_prefix, topic_name, "", qos_policies);
        rid = make_rid(topic);

        //FIXME: qos_policies.history
        //FIXME: qos_policies.durability
        //FIXME: qos_policies.depth
        const bool reliable = (qos_policies->reliability != RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT);

        {
            std::lock_guard<std::mutex> lock(gzhe.lock);
            pub->pubh = zhe_publish(rid, 0, reliable);
        }

        make_gid_from_pubidx(&pub->publisher_gid, pub->pubh);
        make_guid_from_gid(&pub->publisher_guid, &pub->publisher_gid);
        return pub;
    }

    rmw_publisher_t *rmw_create_publisher(const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies)
    {
        ZhePublisher *pub;
        rmw_publisher_t *rmw_publisher;
        if ((pub = create_zhe_publisher(node, type_supports, topic_name, qos_policies)) == nullptr) {
            goto fail_common_init;
        }
        rmw_publisher = rmw_publisher_allocate();
        RET_ALLOC_X(rmw_publisher, goto fail_publisher);
        rmw_publisher->implementation_identifier = adlink_zhe_identifier;
        rmw_publisher->data = pub;
        rmw_publisher->topic_name = reinterpret_cast<char *>(rmw_allocate(strlen(topic_name) + 1));
        RET_ALLOC_X(rmw_publisher->topic_name, goto fail_topic_name);
        memcpy(const_cast<char *>(rmw_publisher->topic_name), topic_name, strlen(topic_name) + 1);
        return rmw_publisher;
    fail_topic_name:
        rmw_publisher_free(rmw_publisher);
        delete pub;
    fail_publisher:
    fail_common_init:
        return nullptr;
    }

    rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t *publisher, rmw_gid_t *gid)
    {
        RET_WRONG_IMPLID(publisher);
        RET_NULL(gid);
        auto pub = static_cast<const ZhePublisher *>(publisher->data);
        RET_NULL(pub);
        gid->implementation_identifier = adlink_zhe_identifier;
        memcpy(gid->data, pub->publisher_gid.data(), sizeof(gid->data));
        return RMW_RET_OK;
    }

    rmw_ret_t rmw_compare_gids_equal(const rmw_gid_t *gid1, const rmw_gid_t *gid2, bool *result)
    {
        RET_WRONG_IMPLID(gid1);
        RET_WRONG_IMPLID(gid2);
        RET_NULL(result);
        *result = memcmp(gid1->data, gid2->data, sizeof(gid1->data)) == 0;
        return RMW_RET_OK;
    }

    rmw_ret_t rmw_destroy_publisher(rmw_node_t *node, rmw_publisher_t *publisher)
    {
        RET_WRONG_IMPLID(node);
        RET_WRONG_IMPLID(publisher);
        auto pub = static_cast<ZhePublisher *>(publisher->data);
        if (pub != nullptr) {
            //FIXME: undeclare Zhe publisher
            delete pub;
        }
        rmw_free(const_cast<char *>(publisher->topic_name));
        publisher->topic_name = nullptr;
        rmw_publisher_free(publisher);
        return RMW_RET_OK;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    SUBSCRIPTIONS                                                  ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////

    static void insert_sample(ZheSubscription *sub, const void *payload, size_t size)
    {
        struct hist *hist = &sub->hist;
        std::lock_guard<std::mutex> lock(sub->cond.internalMutex_);
        assert((hist->count == 0 && hist->newest == nullptr) ||
               (hist->count == 1) == (hist->newest == hist->newest->newer));
        if (hist->count == hist->depth) {
            /* reuse oldest, oldest->data if possible */
            struct histelem *oldest = hist->newest->newer;
            if (oldest->size != size) {
                rmw_free(oldest->data);
                oldest->data = rmw_allocate(size);
            }
            memcpy(oldest->data, payload, size);
            hist->newest = oldest;
        } else {
            auto he = new histelem();
            he->data = rmw_allocate(size);
            he->size = size;
            memcpy(he->data, payload, size);
            struct histelem *prev_newest = hist->newest;
            if (prev_newest == nullptr) {
                he->newer = he;
                condition_set_trigger_locked(&sub->cond, true);
            } else {
                struct histelem *oldest = prev_newest->newer;
                he->newer = oldest;
                prev_newest->newer = he;
            }
            hist->newest = he;
            hist->count++;
        }
    }

    static bool take_sample(ZheSubscription *sub, void **payload, size_t *size)
    {
        struct hist *hist = &sub->hist;
        std::lock_guard<std::mutex> lock(sub->cond.internalMutex_);
        assert((hist->count == 0 && hist->newest == nullptr) ||
               (hist->count == 1) == (hist->newest == hist->newest->newer));
        if (hist->count == 0) {
            return false;
        } else {
            struct histelem *oldest = hist->newest->newer;
            *payload = oldest->data;
            *size = oldest->size;
            if (--hist->count == 0) {
                hist->newest = nullptr;
                condition_set_trigger_locked(&sub->cond, false);
            } else {
                hist->newest->newer = oldest->newer;
            }
            return true;
        }
    }

    static void subhandler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *vsub)
    {
        //NOTE: subhandler called with "gzhe.lock" held
        ZheSubscription *sub = static_cast<ZheSubscription *>(vsub);
        //std::cout << "subhandler rid " << rid << " payload " << payload << " size " << size << " sub " << sub << std::endl;
        (void)rid;
        insert_sample(sub, payload, size);
    }

    static ZheSubscription *create_zhe_subscription(const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies, bool ignore_local_publications)
    {
        RET_WRONG_IMPLID_X(node, return nullptr);
        RET_NULL_OR_EMPTYSTR_X(topic_name, return nullptr);
        RET_NULL_X(qos_policies, return nullptr);
        auto node_impl = static_cast<ZheNode *>(node->data);
        RET_NULL_X(node_impl, return nullptr);
        const rosidl_message_type_support_t *type_support = get_typesupport(type_supports);
        RET_NULL_X(type_support, return nullptr);
        (void)ignore_local_publications;
        ZheSubscription *sub = new ZheSubscription();
        zhe_rid_t rid;

        sub->ts.typesupport_identifier_ = type_support->typesupport_identifier;
        sub->ts.type_support_ = _create_message_type_support(type_support->data, sub->ts.typesupport_identifier_);
        std::string topic = make_fqtopic(ros_topic_prefix, topic_name, "", qos_policies);
        rid = make_rid(topic);

        //FIXME: qos_policies.durability
        //FIXME: qos_policies.reliability
        if (qos_policies->history == RMW_QOS_POLICY_HISTORY_KEEP_ALL) {
            sub->hist.depth = UINT32_MAX;
        } else {
            sub->hist.depth = (qos_policies->depth == RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT) ? 1 : qos_policies->depth;
        }
        sub->hist.count = 0;
        sub->hist.newest = nullptr;

        {
            std::lock_guard<std::mutex> lock(gzhe.lock);
            sub->subh = zhe_subscribe(rid, 0, 0, subhandler, sub);
        }
    
        return sub;
    }

    rmw_subscription_t *rmw_create_subscription(const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies, bool ignore_local_publications)
    {
        ZheSubscription *sub;
        rmw_subscription_t *rmw_subscription;
        if ((sub = create_zhe_subscription(node, type_supports, topic_name, qos_policies, ignore_local_publications)) == nullptr) {
            goto fail_common_init;
        }
        rmw_subscription = rmw_subscription_allocate();
        RET_ALLOC_X(rmw_subscription, goto fail_subscription);
        rmw_subscription->implementation_identifier = adlink_zhe_identifier;
        rmw_subscription->data = sub;
        rmw_subscription->topic_name = reinterpret_cast<const char *>(rmw_allocate(strlen(topic_name) + 1));
        RET_ALLOC_X(rmw_subscription->topic_name, goto fail_topic_name);
        memcpy(const_cast<char *>(rmw_subscription->topic_name), topic_name, strlen(topic_name) + 1);
        return rmw_subscription;
    fail_topic_name:
        rmw_subscription_free(rmw_subscription);
    fail_subscription:
    fail_common_init:
        return nullptr;
    }

    rmw_ret_t rmw_destroy_subscription(rmw_node_t *node, rmw_subscription_t *subscription)
    {
        RET_WRONG_IMPLID(node);
        RET_WRONG_IMPLID(subscription);
        auto sub = static_cast<ZheSubscription *>(subscription->data);
        if (sub != nullptr) {
            //FIXME: undeclare Zhe subscription -- now it will just keep calling the listener and crash
            //FIXME: better be more careful with parallel triggers
            delete sub;
        }
        rmw_free(const_cast<char *>(subscription->topic_name));
        subscription->topic_name = nullptr;
        rmw_subscription_free(subscription);
        return RMW_RET_OK;
    }

    static rmw_ret_t rmw_take_int(const rmw_subscription_t *subscription, void *ros_message, bool *taken, zhe_sample_header_t *header)
    {
        RET_NULL(taken);
        RET_NULL(ros_message);
        RET_WRONG_IMPLID(subscription);
        ZheSubscription *sub = static_cast<ZheSubscription *>(subscription->data);
        RET_NULL(sub);
        void *raw;
        size_t size;
        *taken = take_sample(sub, &raw, &size);
        if (*taken) {
            eprosima::fastcdr::FastBuffer buffer(static_cast<char *>(raw), size);
            eprosima::fastcdr::Cdr deser(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
            desermsg(deser, ros_message, [&header](eprosima::fastcdr::Cdr& deser) { deser >> header->gid; }, sub->ts);
            rmw_free(static_cast<char *>(raw));
        }
        return RMW_RET_OK;
    }

    rmw_ret_t rmw_take(const rmw_subscription_t *subscription, void *ros_message, bool *taken)
    {
        zhe_sample_header_t header;
        return rmw_take_int(subscription, ros_message, taken, &header);
    }

    rmw_ret_t rmw_take_with_info(const rmw_subscription_t *subscription, void *ros_message, bool *taken, rmw_message_info_t *message_info)
    {
        zhe_sample_header_t header;
        rmw_ret_t ret = rmw_take_int(subscription, ros_message, taken, &header);
        if (ret == RMW_RET_OK && *taken) {
            message_info->publisher_gid.implementation_identifier = adlink_zhe_identifier;
            memcpy(message_info->publisher_gid.data, header.gid.data(), sizeof(header.gid));
        }
        return ret;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    GUARDS AND WAITSETS                                            ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////

    struct ZheWaitset {
        std::condition_variable condition;
        std::mutex condition_mutex;
    };

    static void condition_set_trigger_locked(struct condition *cond, bool value)
    {
        if (cond->conditionMutex_ != nullptr) {
            std::unique_lock<std::mutex> clock(*cond->conditionMutex_);
            // the change to hasTriggered_ needs to be mutually exclusive with
            // rmw_wait() which checks hasTriggered() and decides if wait() needs to
            // be called
            cond->hasTriggered_ = value;
            clock.unlock();
            if (value) {
                cond->conditionVariable_->notify_one();
            }
        } else {
            cond->hasTriggered_ = value;
        }
    }

    static void condition_set_trigger(struct condition *cond, bool value)
    {
        std::lock_guard<std::mutex> lock(cond->internalMutex_);
        condition_set_trigger_locked(cond, value);
    }

    static void condition_attach(struct condition *cond, std::mutex *conditionMutex, std::condition_variable *conditionVariable)
    {
        std::lock_guard<std::mutex> lock(cond->internalMutex_);
        cond->conditionMutex_ = conditionMutex;
        cond->conditionVariable_ = conditionVariable;
    }

    static void condition_detach(struct condition *cond)
    {
        std::lock_guard<std::mutex> lock(cond->internalMutex_);
        cond->conditionMutex_ = nullptr;
        cond->conditionVariable_ = nullptr;
    }

    static bool condition_read(const struct condition *cond)
    {
        return cond->hasTriggered_;
    }

    static bool condition_read(struct condition *cond)
    {
        return condition_read(const_cast<const struct condition *>(cond));
    }

    static bool condition_take(struct condition *cond)
    {
        std::lock_guard<std::mutex> lock(cond->internalMutex_);
        bool ret = cond->hasTriggered_;
        cond->hasTriggered_ = false;
        return ret;
    }

    rmw_guard_condition_t *rmw_create_guard_condition()
    {
        rmw_guard_condition_t *guard_condition_handle = new rmw_guard_condition_t;
        guard_condition_handle->implementation_identifier = adlink_zhe_identifier;
        guard_condition_handle->data = new ZheGuardCondition();
        return guard_condition_handle;
    }

    rmw_ret_t rmw_destroy_guard_condition(rmw_guard_condition_t *guard_condition)
    {
        RET_NULL(guard_condition);
        delete static_cast<ZheGuardCondition *>(guard_condition->data);
        delete guard_condition;
        return RMW_RET_OK;
    }

    rmw_ret_t rmw_trigger_guard_condition(const rmw_guard_condition_t *guard_condition_handle)
    {
        RET_WRONG_IMPLID(guard_condition_handle);
        condition_set_trigger(static_cast<ZheGuardCondition *>(guard_condition_handle->data), true);
        return RMW_RET_OK;
    }

    rmw_wait_set_t *rmw_create_wait_set(size_t max_conditions)
    {
        (void)max_conditions;
        rmw_wait_set_t * wait_set = rmw_wait_set_allocate();
        ZheWaitset *ws = nullptr;
        RET_ALLOC_X(wait_set, goto fail_alloc_wait_set);
        wait_set->implementation_identifier = adlink_zhe_identifier;
        wait_set->data = rmw_allocate(sizeof(ZheWaitset));
        RET_ALLOC_X(wait_set->data, goto fail_alloc_wait_set_data);
        // This should default-construct the fields of ZheWaitset
        ws = static_cast<ZheWaitset *>(wait_set->data);
        RMW_TRY_PLACEMENT_NEW(ws, ws, goto fail_placement_new, ZheWaitset, )
        if (!ws) {
            RMW_SET_ERROR_MSG("failed to construct wait set info struct");
            goto fail_ws;
        }
        return wait_set;

    fail_ws:
        RMW_TRY_DESTRUCTOR_FROM_WITHIN_FAILURE(ws->~ZheWaitset(), ws)
    fail_placement_new:
        rmw_free(wait_set->data);
    fail_alloc_wait_set_data:
        rmw_wait_set_free(wait_set);
    fail_alloc_wait_set:
        return nullptr;
    }

    rmw_ret_t rmw_destroy_wait_set(rmw_wait_set_t * wait_set)
    {
        RET_WRONG_IMPLID(wait_set);
        auto result = RMW_RET_OK;
        auto ws = static_cast<ZheWaitset *>(wait_set->data);
        RET_NULL(ws);
        std::mutex *conditionMutex = &ws->condition_mutex;
        RET_NULL(conditionMutex);
        RMW_TRY_DESTRUCTOR(ws->~ZheWaitset(), ws, result = RMW_RET_ERROR)
        rmw_free(wait_set->data);
        rmw_wait_set_free(wait_set);
        return result;
    }

    static bool check_wait_set_for_data(const rmw_subscriptions_t *subs, const rmw_guard_conditions_t *gcs, const rmw_services_t *srvs, const rmw_clients_t *cls)
    {
        if (subs) { for (auto&& x : *subs) { if (condition_read(x)) return true; } }
        if (cls)  { for (auto&& x : *cls)  { if (condition_read(x)) return true; } }
        if (srvs) { for (auto&& x : *srvs) { if (condition_read(x)) return true; } }
        if (gcs)  { for (auto&& x : *gcs)  { if (condition_read(x)) return true; } }
        return false;
    }

    rmw_ret_t rmw_wait(rmw_subscriptions_t *subs, rmw_guard_conditions_t *gcs, rmw_services_t *srvs, rmw_clients_t *cls, rmw_wait_set_t *wait_set, const rmw_time_t *wait_timeout)
    {
        RET_NULL(wait_set);
        ZheWaitset *ws = static_cast<ZheWaitset *>(wait_set->data);
        RET_NULL(ws);
        std::mutex *conditionMutex = &ws->condition_mutex;
        std::condition_variable *conditionVariable = &ws->condition;
        
        if (subs) { for (auto&& x : *subs) condition_attach(x, conditionMutex, conditionVariable); }
        if (cls)  { for (auto&& x : *cls)  condition_attach(x, conditionMutex, conditionVariable); }
        if (srvs) { for (auto&& x : *srvs) condition_attach(x, conditionMutex, conditionVariable); }
        if (gcs)  { for (auto&& x : *gcs)  condition_attach(x, conditionMutex, conditionVariable); }

        // This mutex prevents any of the listeners to change the internal state and notify the
        // condition between the call to hasData() / hasTriggered() and wait() otherwise the
        // decision to wait might be incorrect
        std::unique_lock<std::mutex> lock(*conditionMutex);

        // First check variables.
        // If wait_timeout is null, wait indefinitely (so we have to wait)
        // If wait_timeout is not null and either of its fields are nonzero, we have to wait
        bool timeout;
        if (check_wait_set_for_data(subs, gcs, srvs, cls)) {
            timeout = false;
        } else if (wait_timeout && wait_timeout->sec == 0 && wait_timeout->nsec == 0) {
            /* timeout = 0: no waiting required */
            timeout = true;
        } else {
            auto predicate = [subs, gcs, srvs, cls]() { return check_wait_set_for_data(subs, gcs, srvs, cls); };
            if (!wait_timeout) {
                conditionVariable->wait(lock, predicate);
                timeout = false;
            } else {
                auto n = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(wait_timeout->sec));
                n += std::chrono::nanoseconds(wait_timeout->nsec);
                timeout = !conditionVariable->wait_for(lock, n, predicate);
            }
        }

        // Unlock the condition variable mutex to prevent deadlocks that can occur if
        // a listener triggers while the condition variable is being detached.
        // Listeners will no longer be prevented from changing their internal state,
        // but that should not cause issues (if a listener has data / has triggered
        // after we check, it will be caught on the next call to this function).
        lock.unlock();

        if (subs) { for (auto&& x : *subs) { condition_detach(x); if (!condition_read(x)) x = nullptr; } }
        if (cls)  { for (auto&& x : *cls)  { condition_detach(x); if (!condition_read(x)) x = nullptr; } }
        if (srvs) { for (auto&& x : *srvs) { condition_detach(x); if (!condition_read(x)) x = nullptr; } }
        // guard conditions are auto-resetting, hence condition_take
        if (gcs)  { for (auto&& x : *gcs)  { condition_detach(x); if (!condition_take(x)) x = nullptr; } }

        return timeout ? RMW_RET_TIMEOUT : RMW_RET_OK;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    CLIENTS AND SERVERS                                            ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////

    static rmw_ret_t rmw_take_response_request(ZheCS *cs, rmw_request_id_t *request_header, void *ros_data, bool *taken)
    {
        RET_NULL(taken);
        RET_NULL(ros_data);
        RET_NULL(request_header);
        void *raw;
        size_t size;
        *taken = take_sample(cs->sub, &raw, &size);
        if (*taken) {
            eprosima::fastcdr::FastBuffer buffer(static_cast<char *>(raw), size);
            eprosima::fastcdr::Cdr deser(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
            zhe_request_header_t header;
            desermsg(deser, ros_data, [&header](eprosima::fastcdr::Cdr& deser) { deser >> header.guid; deser >> header.seq; }, cs->sub->ts);
            memcpy(request_header->writer_guid, header.guid.data(), sizeof(header.guid));
            request_header->sequence_number = header.seq;
            rmw_free(static_cast<char *>(raw));
        }
        return RMW_RET_OK;
    }
    
    rmw_ret_t rmw_take_response(const rmw_client_t *client, rmw_request_id_t *request_header, void *ros_response, bool *taken)
    {
        RET_WRONG_IMPLID(client);
        auto info = static_cast<ZheClient *>(client->data);
        return rmw_take_response_request(&info->client, request_header, ros_response, taken);
    }
    
    rmw_ret_t rmw_take_request(const rmw_service_t *service, rmw_request_id_t *request_header, void *ros_request, bool *taken)
    {
        RET_WRONG_IMPLID(service);
        auto info = static_cast<ZheService *>(service->data);
        return rmw_take_response_request(&info->service, request_header, ros_request, taken);
    }

    static rmw_ret_t rmw_send_response_request(ZheCS *cs, zhe_request_header_t *header, const void *ros_data)
    {
        eprosima::fastcdr::FastBuffer buffer;
        eprosima::fastcdr::Cdr ser(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
        if (sermsg(ros_data, ser, [&header](eprosima::fastcdr::Cdr& ser) { ser << header->guid; ser << header->seq; }, cs->pub->ts)) {
            return rmw_write_ser(cs->pub->pubh, ser);
        } else {
            RMW_SET_ERROR_MSG("cannot serialize data");
            return RMW_RET_ERROR;
        }
    }

    rmw_ret_t rmw_send_response(const rmw_service_t *service, rmw_request_id_t *request_header, void *ros_response)
    {
        RET_WRONG_IMPLID(service);
        RET_NULL(request_header);
        RET_NULL(ros_response);
        ZheService *info = static_cast<ZheService *>(service->data);
        zhe_request_header_t header;
        memcpy(header.guid.data(), request_header->writer_guid, sizeof(header.guid));
        header.seq = request_header->sequence_number;
        return rmw_send_response_request(&info->service, &header, ros_response);
    }

    rmw_ret_t rmw_send_request(const rmw_client_t *client, const void *ros_request, int64_t *sequence_id)
    {
        RET_WRONG_IMPLID(client);
        RET_NULL(ros_request);
        RET_NULL(sequence_id);
        auto info = static_cast<ZheClient *>(client->data);
        zhe_request_header_t header;
        header.guid = info->client.pub->publisher_guid;
        header.seq = *sequence_id = gzhe.next_request_id++;
        return rmw_send_response_request(&info->client, &header, ros_request);
    }

    static void clienthandler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *vcs)
    {
        auto cs = static_cast<ZheCS *>(vcs);
        eprosima::fastcdr::FastBuffer buffer(static_cast<char *>(const_cast<void *>(payload)), size);
        eprosima::fastcdr::Cdr deser(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
        zhe_request_header_t header;
        (void)rid;
        deser.read_encapsulation();
        deser >> header.guid;
        if (header.guid == cs->pub->publisher_guid) {
            subhandler(rid, payload, size, cs->sub);
        }
    }

    static void servicehandler(zhe_rid_t rid, const void *payload, zhe_paysize_t size, void *vcs)
    {
        auto cs = static_cast<ZheCS *>(vcs);
        subhandler(rid, payload, size, cs->sub);
    }
    
#if 0
    static const void *get_request_ptr(const void *untyped_service_members, const char *typesupport)
    {
        if (using_introspection_c_typesupport(typesupport)) {
            return get_request_ptr<rosidl_typesupport_introspection_c__ServiceMembers>(untyped_service_members);
        } else if (using_introspection_cpp_typesupport(typesupport)) {
            return get_request_ptr<rosidl_typesupport_introspection_cpp::ServiceMembers>(untyped_service_members);
        }
        RMW_SET_ERROR_MSG("Unknown typesupport identifier");
        return nullptr;
    }

    static const void *get_response_ptr(const void *untyped_service_members, const char *typesupport)
    {
        if (using_introspection_c_typesupport(typesupport)) {
            return get_response_ptr<rosidl_typesupport_introspection_c__ServiceMembers>(untyped_service_members);
        } else if (using_introspection_cpp_typesupport(typesupport)) {
            return get_response_ptr<rosidl_typesupport_introspection_cpp::ServiceMembers>(untyped_service_members);
        }
        RMW_SET_ERROR_MSG("Unknown typesupport identifier");
        return nullptr;
    }
#endif

    static const rosidl_service_type_support_t *get_service_typesupport(const rosidl_service_type_support_t *type_supports)
    {
        const rosidl_service_type_support_t *ts;
        if ((ts = get_service_typesupport_handle(type_supports, rosidl_typesupport_introspection_c__identifier)) != nullptr) {
            return ts;
        } else if ((ts = get_service_typesupport_handle(type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr) {
            return ts;
        } else {
            RMW_SET_ERROR_MSG("service type support not from this implementation");
            return nullptr;
        }
    }
    
    static rmw_ret_t rmw_init_cs(ZheCS *cs, const rmw_node_t *node, const rosidl_service_type_support_t *type_supports, const char *service_name, const rmw_qos_profile_t *qos_policies, bool is_service)
    {
        RET_WRONG_IMPLID(node);
        RET_NULL_OR_EMPTYSTR(service_name);
        RET_NULL(qos_policies);
        auto node_impl = static_cast<ZheNode *>(node->data);
        RET_NULL(node_impl);
        const rosidl_service_type_support_t *type_support = get_service_typesupport(type_supports);
        RET_NULL(type_support);

        auto pub = new ZhePublisher();
        auto sub = new ZheSubscription();
        std::string subtopic, pubtopic;
        pub->ts.typesupport_identifier_ = type_support->typesupport_identifier;
        sub->ts.typesupport_identifier_ = type_support->typesupport_identifier;
        if (is_service) {
            //const void *untyped_sub_members = get_request_ptr(type_support->data, type_support->typesupport_identifier);
            //const void *untyped_pub_members = get_response_ptr(type_support->data, type_support->typesupport_identifier);
            sub->ts.type_support_ = _create_request_type_support(type_support->data, type_support->typesupport_identifier);
            pub->ts.type_support_ = _create_response_type_support(type_support->data, type_support->typesupport_identifier);
            subtopic = make_fqtopic(ros_service_requester_prefix, service_name, "_request", qos_policies);
            pubtopic = make_fqtopic(ros_service_response_prefix, service_name, "_reply", qos_policies);
        } else {
            //const void *untyped_pub_members = get_request_ptr(type_support->data, type_support->typesupport_identifier);
            //const void *untyped_sub_members = get_response_ptr(type_support->data, type_support->typesupport_identifier);
            pub->ts.type_support_ = _create_request_type_support(type_support->data, type_support->typesupport_identifier);
            sub->ts.type_support_ = _create_response_type_support(type_support->data, type_support->typesupport_identifier);
            pubtopic = make_fqtopic(ros_service_requester_prefix, service_name, "_request", qos_policies);
            subtopic = make_fqtopic(ros_service_response_prefix, service_name, "_reply", qos_policies);
        }

        zhe_rid_t rid_sub = make_rid(subtopic);
        zhe_rid_t rid_pub = make_rid(pubtopic);

        sub->hist.depth = UINT32_MAX;
        sub->hist.count = 0;
        sub->hist.newest = nullptr;
        
        RCUTILS_LOG_DEBUG_NAMED("rmw_zhe_cpp", "************ %s Details *********", is_service ? "Service" : "Client")
        RCUTILS_LOG_DEBUG_NAMED("rmw_zhe_cpp", "Sub Topic %s", subtopic.c_str())
        RCUTILS_LOG_DEBUG_NAMED("rmw_zhe_cpp", "Pub Topic %s", pubtopic.c_str())
        RCUTILS_LOG_DEBUG_NAMED("rmw_zhe_cpp", "***********")

        {
            std::lock_guard<std::mutex> lock(gzhe.lock);
            sub->subh = zhe_subscribe(rid_sub, 0, 0, is_service ? servicehandler : clienthandler, cs);
            pub->pubh = zhe_publish(rid_pub, 0, true);
            make_gid_from_pubidx(&pub->publisher_gid, pub->pubh);
            make_guid_from_gid(&pub->publisher_guid, &pub->publisher_gid);
        }

        cs->pub = pub;
        cs->sub = sub;
        return RMW_RET_OK;
    }

    rmw_client_t *rmw_create_client(const rmw_node_t *node, const rosidl_service_type_support_t *type_supports, const char *service_name, const rmw_qos_profile_t *qos_policies)
    {
        ZheClient *info = new ZheClient();
        if (rmw_init_cs(&info->client, node, type_supports, service_name, qos_policies, false) != RMW_RET_OK) {
            //return nullptr;
        }
        rmw_client_t *rmw_client = rmw_client_allocate();
        RET_NULL_X(rmw_client, goto fail_client);
        rmw_client->implementation_identifier = adlink_zhe_identifier;
        rmw_client->data = info;
        rmw_client->service_name = reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
        RET_NULL_X(rmw_client->service_name, goto fail_service_name);
        memcpy(const_cast<char *>(rmw_client->service_name), service_name, strlen(service_name) + 1);
        return rmw_client;
    fail_service_name:
        rmw_client_free(rmw_client);
    fail_client:
        // FIXME: can't undo the damage done ...
        return nullptr;
    }

    rmw_ret_t rmw_destroy_client(rmw_node_t *node, rmw_client_t *client)
    {
        RET_WRONG_IMPLID(node);
        RET_WRONG_IMPLID(client);
        auto info = static_cast<ZheClient *>(client->data);
        (void)info;
        // FIXME: can't delete all the stuff in Zhe ...
        rmw_free(const_cast<char *>(client->service_name));
        rmw_client_free(client);
        return RMW_RET_OK;
    }

    rmw_service_t *rmw_create_service(const rmw_node_t *node, const rosidl_service_type_support_t *type_supports, const char *service_name, const rmw_qos_profile_t *qos_policies)
    {
        ZheService *info = new ZheService();
        if (rmw_init_cs(&info->service, node, type_supports, service_name, qos_policies, true) != RMW_RET_OK) {
            //return nullptr;
        }
        rmw_service_t *rmw_service = rmw_service_allocate();
        RET_NULL_X(rmw_service, goto fail_service);
        rmw_service->implementation_identifier = adlink_zhe_identifier;
        rmw_service->data = info;
        rmw_service->service_name = reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
        RET_NULL_X(rmw_service->service_name, goto fail_service_name);
        memcpy(const_cast<char *>(rmw_service->service_name), service_name, strlen(service_name) + 1);
        return rmw_service;
    fail_service_name:
        rmw_service_free(rmw_service);
    fail_service:
        // FIXME: can't undo the damage done ...
        return nullptr;
    }

    rmw_ret_t rmw_destroy_service(rmw_node_t *node, rmw_service_t *service)
    {
        RET_WRONG_IMPLID(node);
        RET_WRONG_IMPLID(service);
        auto info = static_cast<ZheService *>(service->data);
        (void)info;
        // FIXME: can't delete all the stuff in Zhe ...
        rmw_free(const_cast<char *>(service->service_name));
        rmw_service_free(service);
        return RMW_RET_OK;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    INTROSPECTION                                                  ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////
  
    rmw_ret_t rmw_get_node_names(const rmw_node_t *node, rcutils_string_array_t *node_names)
    {
#if 0 // NIY
        RET_WRONG_IMPLID(node);
        if (rmw_check_zero_rmw_string_array(node_names) != RMW_RET_OK) {
            return RMW_RET_ERROR;
        }

        auto impl = static_cast<ZheNode *>(node->data);

        // FIXME: sorry, can't do it with current Zenoh
        auto participant_names = std::vector<std::string>{};
        rcutils_allocator_t allocator = rcutils_get_default_allocator();
        rcutils_ret_t rcutils_ret =
            rcutils_string_array_init(node_names, participant_names.size(), &allocator);
        if (rcutils_ret != RCUTILS_RET_OK) {
            RMW_SET_ERROR_MSG(rcutils_get_error_string_safe())
            return rmw_convert_rcutils_ret_to_rmw_ret(rcutils_ret);
        }
        for (size_t i = 0; i < participant_names.size(); ++i) {
            node_names->data[i] = rcutils_strdup(participant_names[i].c_str(), allocator);
            if (!node_names->data[i]) {
                RMW_SET_ERROR_MSG("failed to allocate memory for node name")
                    rcutils_ret = rcutils_string_array_fini(node_names);
                if (rcutils_ret != RCUTILS_RET_OK) {
                    RCUTILS_LOG_ERROR_NAMED(
                            "rmw_zhe_cpp",
                            "failed to cleanup during error handling: %s", rcutils_get_error_string_safe())
                        }
                return RMW_RET_BAD_ALLOC;
            }
        }
        return RMW_RET_OK;
#else
        (void)node; (void)node_names;
        return RMW_RET_TIMEOUT;
#endif
    }

    rmw_ret_t rmw_get_topic_names_and_types(const rmw_node_t *node, rcutils_allocator_t *allocator, bool no_demangle, rmw_names_and_types_t *topic_names_and_types)
    {
#if 0 // NIY
        RET_NULL(allocator);
        RET_WRONG_IMPLID(node);
        rmw_ret_t ret = rmw_names_and_types_check_zero(topic_names_and_types);
        if (ret != RMW_RET_OK) {
            return ret;
        }

        auto impl = static_cast<CustomParticipantInfo *>(node->data);

        // Access the slave Listeners, which are the ones that have the topicnamesandtypes member
        // Get info from publisher and subscriber
        // Combined results from the two lists
        std::map<std::string, std::set<std::string>> topics;
        {
            ReaderInfo * slave_target = impl->secondarySubListener;
            slave_target->mapmutex.lock();
            for (auto it : slave_target->topicNtypes) {
                if (!no_demangle && _get_ros_prefix_if_exists(it.first) != ros_topic_prefix) {
                    // if we are demangling and this is not prefixed with rt/, skip it
                    continue;
                }
                for (auto & itt : it.second) {
                    topics[it.first].insert(itt);
                }
            }
            slave_target->mapmutex.unlock();
        }
        {
            WriterInfo * slave_target = impl->secondaryPubListener;
            slave_target->mapmutex.lock();
            for (auto it : slave_target->topicNtypes) {
                if (!no_demangle && _get_ros_prefix_if_exists(it.first) != ros_topic_prefix) {
                    // if we are demangling and this is not prefixed with rt/, skip it
                    continue;
                }
                for (auto & itt : it.second) {
                    topics[it.first].insert(itt);
                }
            }
            slave_target->mapmutex.unlock();
        }

        // Copy data to results handle
        if (topics.size() > 0) {
            // Setup string array to store names
            rmw_ret_t rmw_ret = rmw_names_and_types_init(topic_names_and_types, topics.size(), allocator);
            if (rmw_ret != RMW_RET_OK) {
                return rmw_ret;
            }
            // Setup cleanup function, in case of failure below
            auto fail_cleanup = [&topic_names_and_types]() {
                                    rmw_ret_t rmw_ret = rmw_names_and_types_fini(topic_names_and_types);
                                    if (rmw_ret != RMW_RET_OK) {
                                        RCUTILS_LOG_ERROR_NAMED(
                                                "rmw_zhe_cpp",
                                                "error during report of error: %s", rmw_get_error_string_safe())
                                            }
                                };
            // Setup demangling functions based on no_demangle option
            auto demangle_topic = _demangle_if_ros_topic;
            auto demangle_type = _demangle_if_ros_type;
            if (no_demangle) {
                auto noop = [](const std::string & in) {
                                return in;
                            };
                demangle_topic = noop;
                demangle_type = noop;
            }
            // For each topic, store the name, initialize the string array for types, and store all types
            size_t index = 0;
            for (const auto & topic_n_types : topics) {
                // Duplicate and store the topic_name
                char * topic_name = rcutils_strdup(demangle_topic(topic_n_types.first).c_str(), *allocator);
                if (!topic_name) {
                    RMW_SET_ERROR_MSG_ALLOC("failed to allocate memory for topic name", *allocator);
                    fail_cleanup();
                    return RMW_RET_BAD_ALLOC;
                }
                topic_names_and_types->names.data[index] = topic_name;
                // Setup storage for types
                {
                    rcutils_ret_t rcutils_ret = rcutils_string_array_init(
                            &topic_names_and_types->types[index],
                            topic_n_types.second.size(),
                            allocator);
                    if (rcutils_ret != RCUTILS_RET_OK) {
                        RMW_SET_ERROR_MSG(rcutils_get_error_string_safe())
                            fail_cleanup();
                        return rmw_convert_rcutils_ret_to_rmw_ret(rcutils_ret);
                    }
                }
                // Duplicate and store each type for the topic
                size_t type_index = 0;
                for (const auto & type : topic_n_types.second) {
                    char * type_name = rcutils_strdup(demangle_type(type).c_str(), *allocator);
                    if (!type_name) {
                        RMW_SET_ERROR_MSG_ALLOC("failed to allocate memory for type name", *allocator)
                            fail_cleanup();
                        return RMW_RET_BAD_ALLOC;
                    }
                    topic_names_and_types->types[index].data[type_index] = type_name;
                    ++type_index;
                }  // for each type
                ++index;
            }  // for each topic
        }
        return RMW_RET_OK;
#else
        (void)node; (void)allocator; (void)no_demangle; (void)topic_names_and_types;
        return RMW_RET_TIMEOUT;
#endif
    }

    rmw_ret_t rmw_get_service_names_and_types(const rmw_node_t *node, rcutils_allocator_t *allocator, rmw_names_and_types_t *service_names_and_types)
    {
#if 0 // NIY
        if (!allocator) {
            RMW_SET_ERROR_MSG("allocator is null")
                return RMW_RET_INVALID_ARGUMENT;
        }
        if (!node) {
            RMW_SET_ERROR_MSG_ALLOC("null node handle", *allocator)
                return RMW_RET_INVALID_ARGUMENT;
        }
        rmw_ret_t ret = rmw_names_and_types_check_zero(service_names_and_types);
        if (ret != RMW_RET_OK) {
            return ret;
        }

        // Get participant pointer from node
        if (node->implementation_identifier != adlink_zhe_identifier) {
            RMW_SET_ERROR_MSG_ALLOC("node handle not from this implementation", *allocator);
            return RMW_RET_ERROR;
        }

        auto impl = static_cast<CustomParticipantInfo *>(node->data);

        // Access the slave Listeners, which are the ones that have the topicnamesandtypes member
        // Get info from publisher and subscriber
        // Combined results from the two lists
        std::map<std::string, std::set<std::string>> services;
        {
            ReaderInfo * slave_target = impl->secondarySubListener;
            slave_target->mapmutex.lock();
            for (auto it : slave_target->topicNtypes) {
                std::string service_name = _demangle_service_from_topic(it.first);
                if (!service_name.length()) {
                    // not a service
                    continue;
                }
                for (auto & itt : it.second) {
                    std::string service_type = _demangle_service_type_only(itt);
                    if (service_type.length()) {
                        services[service_name].insert(service_type);
                    }
                }
            }
            slave_target->mapmutex.unlock();
        }
        {
            WriterInfo * slave_target = impl->secondaryPubListener;
            slave_target->mapmutex.lock();
            for (auto it : slave_target->topicNtypes) {
                std::string service_name = _demangle_service_from_topic(it.first);
                if (!service_name.length()) {
                    // not a service
                    continue;
                }
                for (auto & itt : it.second) {
                    std::string service_type = _demangle_service_type_only(itt);
                    if (service_type.length()) {
                        services[service_name].insert(service_type);
                    }
                }
            }
            slave_target->mapmutex.unlock();
        }

        // Fill out service_names_and_types
        if (services.size()) {
            // Setup string array to store names
            rmw_ret_t rmw_ret =
                rmw_names_and_types_init(service_names_and_types, services.size(), allocator);
            if (rmw_ret != RMW_RET_OK) {
                return rmw_ret;
            }
            // Setup cleanup function, in case of failure below
            auto fail_cleanup = [&service_names_and_types]() {
                                    rmw_ret_t rmw_ret = rmw_names_and_types_fini(service_names_and_types);
                                    if (rmw_ret != RMW_RET_OK) {
                                        RCUTILS_LOG_ERROR_NAMED(
                                                "rmw_zhe_cpp",
                                                "error during report of error: %s", rmw_get_error_string_safe())
                                            }
                                };
            // For each service, store the name, initialize the string array for types, and store all types
            size_t index = 0;
            for (const auto & service_n_types : services) {
                // Duplicate and store the service_name
                char * service_name = rcutils_strdup(service_n_types.first.c_str(), *allocator);
                if (!service_name) {
                    RMW_SET_ERROR_MSG_ALLOC("failed to allocate memory for service name", *allocator);
                    fail_cleanup();
                    return RMW_RET_BAD_ALLOC;
                }
                service_names_and_types->names.data[index] = service_name;
                // Setup storage for types
                {
                    rcutils_ret_t rcutils_ret = rcutils_string_array_init(
                            &service_names_and_types->types[index],
                            service_n_types.second.size(),
                            allocator);
                    if (rcutils_ret != RCUTILS_RET_OK) {
                        RMW_SET_ERROR_MSG(rcutils_get_error_string_safe())
                            fail_cleanup();
                        return rmw_convert_rcutils_ret_to_rmw_ret(rcutils_ret);
                    }
                }
                // Duplicate and store each type for the service
                size_t type_index = 0;
                for (const auto & type : service_n_types.second) {
                    char * type_name = rcutils_strdup(type.c_str(), *allocator);
                    if (!type_name) {
                        RMW_SET_ERROR_MSG_ALLOC("failed to allocate memory for type name", *allocator)
                            fail_cleanup();
                        return RMW_RET_BAD_ALLOC;
                    }
                    service_names_and_types->types[index].data[type_index] = type_name;
                    ++type_index;
                }  // for each type
                ++index;
            }  // for each service
        }
        return RMW_RET_OK;
#else
        (void)node; (void)allocator; (void)service_names_and_types;
        return RMW_RET_TIMEOUT;
#endif
    }

    rmw_ret_t rmw_service_server_is_available(const rmw_node_t * node, const rmw_client_t * client, bool * is_available)
    {
#if 0 // NIY
        if (!node) {
            RMW_SET_ERROR_MSG("node handle is null");
            return RMW_RET_ERROR;
        }

        RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
                node handle,
                node->implementation_identifier, adlink_zhe_identifier,
                return RMW_RET_ERROR);

        if (!client) {
            RMW_SET_ERROR_MSG("client handle is null");
            return RMW_RET_ERROR;
        }

        if (!is_available) {
            RMW_SET_ERROR_MSG("is_available is null");
            return RMW_RET_ERROR;
        }

        auto client_info = static_cast<CustomClientInfo *>(client->data);
        if (!client_info) {
            RMW_SET_ERROR_MSG("client info handle is null");
            return RMW_RET_ERROR;
        }

        auto pub_topic_name =
            client_info->request_publisher_->getAttributes().topic.getTopicName();
        auto pub_partitions =
            client_info->request_publisher_->getAttributes().qos.m_partition.getNames();
        // every rostopic has exactly 1 partition field set
        if (pub_partitions.size() != 1) {
            RCUTILS_LOG_ERROR_NAMED(
                    "rmw_zhe_cpp",
                    "Topic %s is not a ros topic", pub_topic_name.c_str())
                RMW_SET_ERROR_MSG((std::string(pub_topic_name) + " is a non-ros topic\n").c_str());
            return RMW_RET_ERROR;
        }
        auto pub_fqdn = pub_partitions[0] + "/" + pub_topic_name;
        pub_fqdn = _demangle_if_ros_topic(pub_fqdn);

        auto sub_topic_name =
            client_info->response_subscriber_->getAttributes().topic.getTopicName();
        auto sub_partitions =
            client_info->response_subscriber_->getAttributes().qos.m_partition.getNames();
        // every rostopic has exactly 1 partition field set
        if (sub_partitions.size() != 1) {
            RCUTILS_LOG_ERROR_NAMED(
                    "rmw_zhe_cpp",
                    "Topic %s is not a ros topic", pub_topic_name.c_str())
                RMW_SET_ERROR_MSG((std::string(sub_topic_name) + " is a non-ros topic\n").c_str());
            return RMW_RET_ERROR;
        }
        auto sub_fqdn = sub_partitions[0] + "/" + sub_topic_name;
        sub_fqdn = _demangle_if_ros_topic(sub_fqdn);

        *is_available = false;
        size_t number_of_request_subscribers = 0;
        rmw_ret_t ret = rmw_count_subscribers(
                node,
                pub_fqdn.c_str(),
                &number_of_request_subscribers);
        if (ret != RMW_RET_OK) {
            // error string already set
            return ret;
        }
        if (number_of_request_subscribers == 0) {
            // not ready
            return RMW_RET_OK;
        }

        size_t number_of_response_publishers = 0;
        ret = rmw_count_publishers(
                node,
                sub_fqdn.c_str(),
                &number_of_response_publishers);
        if (ret != RMW_RET_OK) {
            // error string already set
            return ret;
        }
        if (number_of_response_publishers == 0) {
            // not ready
            return RMW_RET_OK;
        }

        // all conditions met, there is a service server available
        *is_available = true;
        return RMW_RET_OK;
#else
        (void)node; (void)client; (void)is_available;
        return RMW_RET_TIMEOUT;
#endif
    }
    
    rmw_ret_t rmw_count_publishers(const rmw_node_t *node, const char *topic_name, size_t *count)
    {
#if 0
        // safechecks

        if (!node) {
            RMW_SET_ERROR_MSG("null node handle");
            return RMW_RET_ERROR;
        }
        // Get participant pointer from node
        if (node->implementation_identifier != eprosima_fastrtps_identifier) {
            RMW_SET_ERROR_MSG("node handle not from this implementation");
            return RMW_RET_ERROR;
        }

        auto impl = static_cast<CustomParticipantInfo *>(node->data);

        WriterInfo * slave_target = impl->secondaryPubListener;
        slave_target->mapmutex.lock();
        *count = 0;
        for (auto it : slave_target->topicNtypes) {
            auto topic_fqdn = _demangle_if_ros_topic(it.first);
            if (topic_fqdn == topic_name) {
                *count += it.second.size();
            }
        }
        slave_target->mapmutex.unlock();

        RCUTILS_LOG_DEBUG_NAMED(
                "rmw_fastrtps_cpp",
                "looking for subscriber topic: %s, number of matches: %zu",
                topic_name, *count)

        return RMW_RET_OK;
#else
        (void)node; (void)topic_name; (void)count;
        return RMW_RET_TIMEOUT;
#endif
    }

    rmw_ret_t rmw_count_subscribers(const rmw_node_t *node, const char *topic_name, size_t *count)
    {
#if 0
        // safechecks

        if (!node) {
            RMW_SET_ERROR_MSG("null node handle");
            return RMW_RET_ERROR;
        }
        // Get participant pointer from node
        if (node->implementation_identifier != eprosima_fastrtps_identifier) {
            RMW_SET_ERROR_MSG("node handle not from this implementation");
            return RMW_RET_ERROR;
        }

        CustomParticipantInfo * impl = static_cast<CustomParticipantInfo *>(node->data);

        ReaderInfo * slave_target = impl->secondarySubListener;
        *count = 0;
        slave_target->mapmutex.lock();
        for (auto it : slave_target->topicNtypes) {
            auto topic_fqdn = _demangle_if_ros_topic(it.first);
            if (topic_fqdn == topic_name) {
                *count += it.second.size();
            }
        }
        slave_target->mapmutex.unlock();

        RCUTILS_LOG_DEBUG_NAMED(
                "rmw_fastrtps_cpp",
                "looking for subscriber topic: %s, number of matches: %zu",
                topic_name, *count)

        return RMW_RET_OK;
#else
        (void)node; (void)topic_name; (void)count;
        return RMW_RET_TIMEOUT;
#endif
    }
}  // extern "C"
