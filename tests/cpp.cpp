// SPDX-License-Identifier: LGPL-2.1-or-later
#include <libipc/ipc.hpp>
#include "process.h"
#include <cassert>
#include <string>
static int interoperates(const char *peer) {
    test_process process = {};
    std::string name = "cppinterop" + std::to_string(test_process_id());
    assert(test_start_echo(peer, name.c_str(), &process));
    auto context = libipc::make_context();
    libipc::channel client;
    libipc::connect(context, name.c_str(), [&](libipc::completion r) {
        assert(r.status == IPC_OK); client = std::move(r.connection);
    }, 1000);
    for (int i = 0; !client && i < 100; ++i) assert(context.run_once(10) >= 0);
    assert(client);
    bool sent = false, received = false;
    libipc::send(client, "C++ to C", 8, [&](libipc::completion r) { assert(r.status == IPC_OK); sent = true; }, 1000);
    libipc::receive(client, [&](libipc::completion r) {
        assert(r.status == IPC_OK && r.payload && r.payload->size == 8);
        assert(std::string(static_cast<char *>(r.payload->data), r.payload->size) == "C++ to C"); received = true;
    }, 1000);
    for (int i = 0; (!sent || !received) && i < 100; ++i) assert(context.run_once(10) >= 0);
    assert(sent && received && test_wait_process(&process)); return 0;
}
int main(int argc, char **argv) {
    if (argc == 2) return interoperates(argv[1]);
    auto context = libipc::make_context();
    assert(context.run_once(0) >= 0);
    std::string name = "cppownership" + std::to_string(test_process_id());
    auto listener = libipc::listen(context, name.c_str());
    libipc::channel server, client;
    libipc::accept(listener, [&](libipc::completion r) { assert(r.status == IPC_OK); server = std::move(r.connection); }, 1000);
    libipc::connect(context, name.c_str(), [&](libipc::completion r) { assert(r.status == IPC_OK); client = std::move(r.connection); }, 1000);
    for (int i = 0; (!server || !client) && i < 100; ++i) assert(context.run_once(10) >= 0);
    assert(server && client);
    bool sent = false, received = false;
    libipc::send(client, "hello", 5, [&](libipc::completion r) { assert(r.status == IPC_OK); sent = true; }, 1000);
    libipc::receive(server, [&](libipc::completion r) {
        assert(r.status == IPC_OK && r.payload && r.payload->size == 5);
        received = true;
    }, 1000);
    for (int i = 0; (!sent || !received) && i < 100; ++i) assert(context.run_once(10) >= 0);
    assert(sent && received);
    return 0;
}
