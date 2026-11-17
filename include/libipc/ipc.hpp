// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "ipc.h"
#include <memory>
#include <stdexcept>
#include <functional>
#include <exception>
#include <utility>
namespace libipc {
inline void check(ipc_status status) {
    if (status != IPC_OK) throw std::runtime_error("libipc operation failed");
}
struct context_deleter { void operator()(ipc_context *p) const { ipc_context_destroy(p); } };
struct region_deleter { void operator()(ipc_region *p) const { ipc_region_close(p); } };
struct sync_deleter { void operator()(ipc_sync *p) const { ipc_sync_close(p); } };
struct message_deleter { void operator()(ipc_message *p) const { ipc_message_release(p); } };
using message = std::unique_ptr<ipc_message, message_deleter>;
class context {
    std::shared_ptr<ipc_context> owner_;
public:
    explicit context(const ipc_options *options = nullptr) {
        ipc_context *p = nullptr;
        check(ipc_context_create(options, &p));
        owner_ = std::shared_ptr<ipc_context>(p, context_deleter{});
    }
    ipc_context *get() const noexcept { return owner_.get(); }
    std::shared_ptr<ipc_context> share() const { return owner_; }
    int run_once(int timeout = -1) const {
        auto keep_alive = owner_;
        return ipc_run_once(keep_alive.get(), timeout);
    }
    void run() const { auto keep_alive = owner_; ipc_run(keep_alive.get()); }
    void stop() const { ipc_stop(get()); }
    void cancel(ipc_operation id) const { check(ipc_cancel(get(), id)); }
};
inline context make_context(const ipc_options *options = nullptr) { return context(options); }

template<class T, void (*Close)(T *)> class handle {
    std::shared_ptr<ipc_context> owner_;
    struct deleter { void operator()(T *p) const { Close(p); } };
    std::unique_ptr<T, deleter> native_;
public:
    handle() = default;
    handle(T *p, std::shared_ptr<ipc_context> owner = {}) : owner_(std::move(owner)), native_(p) {}
    handle(handle &&) noexcept = default;
    handle &operator=(handle other) noexcept {
        owner_.swap(other.owner_); native_.swap(other.native_); return *this;
    }
    T *get() const noexcept { return native_.get(); }
    std::shared_ptr<ipc_context> owner() const { return owner_; }
    explicit operator bool() const noexcept { return bool(native_); }
};
using channel = handle<ipc_channel, ipc_channel_close>;
using listener = handle<ipc_listener, ipc_listener_close>;
using region = handle<ipc_region, ipc_region_close>;
struct completion {
    ipc_operation operation;
    ipc_status status;
    channel connection;
    message payload;
};
// Called on the context's driver thread. An escaping exception terminates.
using callback = std::function<void(completion)>;
namespace detail {
struct callback_data {
    std::weak_ptr<ipc_context> owner;
    callback function;
};
inline void dispatch(const ipc_result *r, void *user) noexcept {
    std::unique_ptr<callback_data> data(static_cast<callback_data *>(user));
    try {
        data->function(completion{r->operation, r->status,
            channel(r->channel, data->owner.lock()), message(r->message)});
    } catch (...) { std::terminate(); }
}
template<class Submit> ipc_operation request(std::shared_ptr<ipc_context> owner,
                                            callback cb, Submit submit) {
    if (!cb) throw std::invalid_argument("empty IPC callback");
    auto data = std::make_unique<callback_data>(callback_data{owner, std::move(cb)});
    ipc_operation id = 0;
    check(submit(dispatch, data.get(), &id));
    data.release(); return id;
}
}
inline listener listen(const context &c, const char *name) {
    ipc_listener *p = nullptr;
    check(ipc_listen(c.get(), name, &p)); return listener(p, c.share());
}
inline ipc_operation connect(const context &c, const char *name, callback cb, int timeout = -1) {
    return detail::request(c.share(), std::move(cb), [&](auto fn, auto user, auto id) {
        return ipc_connect(c.get(), name, timeout, fn, user, id);
    });
}
inline ipc_operation accept(const listener &l, callback cb, int timeout = -1) {
    return detail::request(l.owner(), std::move(cb), [&](auto fn, auto user, auto id) {
        return ipc_accept(l.get(), timeout, fn, user, id);
    });
}
inline ipc_operation send(const channel &c, const void *data, std::size_t size, callback cb, int timeout = -1) {
    return detail::request(c.owner(), std::move(cb), [&](auto fn, auto user, auto id) {
        return ipc_send(c.get(), data, size, timeout, fn, user, id);
    });
}
inline ipc_operation receive(const channel &c, callback cb, int timeout = -1) {
    return detail::request(c.owner(), std::move(cb), [&](auto fn, auto user, auto id) {
        return ipc_receive(c.get(), timeout, fn, user, id);
    });
}
inline region create_region(const char *name, std::size_t size) {
    ipc_region *p = nullptr; check(ipc_region_create(name, size, &p)); return region(p);
}
inline region open_region(const char *name) {
    ipc_region *p = nullptr; check(ipc_region_open(name, &p)); return region(p);
}
class lock_guard {
    std::shared_ptr<ipc_sync> sync_;
public:
    lock_guard() = default;
    explicit lock_guard(std::shared_ptr<ipc_sync> p) : sync_(std::move(p)) {}
    lock_guard(lock_guard &&) noexcept = default;
    lock_guard &operator=(lock_guard other) noexcept { sync_.swap(other.sync_); return *this; }
    ~lock_guard() { if (sync_) ipc_sync_post(sync_.get()); }
    explicit operator bool() const noexcept { return bool(sync_); }
};
class synchronizer {
    std::shared_ptr<ipc_context> owner_;
    std::shared_ptr<ipc_sync> sync_;
    synchronizer(const context &c, ipc_sync *p) : owner_(c.share()), sync_(p, sync_deleter{}) {}
public:
    static synchronizer create(const context &c, const char *name, unsigned initial = 0) {
        ipc_sync *p = nullptr; check(ipc_sync_create(name, initial, &p)); return synchronizer(c, p);
    }
    static synchronizer open(const context &c, const char *name) {
        ipc_sync *p = nullptr; check(ipc_sync_open(name, &p)); return synchronizer(c, p);
    }
    ipc_sync *get() const noexcept { return sync_.get(); }
    void post() const { check(ipc_sync_post(get())); }
    ipc_status try_wait() const { return ipc_sync_try_wait(get()); }
    ipc_operation wait(callback cb, int timeout = -1) const {
        return detail::request(owner_, std::move(cb), [&](auto fn, auto user, auto id) {
            return ipc_sync_wait(owner_.get(), get(), timeout, fn, user, id);
        });
    }
    ipc_operation lock(std::function<void(ipc_status, lock_guard)> cb, int timeout = -1) const {
        if (!cb) throw std::invalid_argument("empty IPC lock callback");
        return wait([sync = sync_, cb = std::move(cb)](completion r) {
            cb(r.status, r.status == IPC_OK ? lock_guard(sync) : lock_guard());
        }, timeout);
    }
};
}
