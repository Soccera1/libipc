// SPDX-License-Identifier: LGPL-2.1-or-later
fn main() {
    println!("cargo:rerun-if-env-changed=LIBIPC_TEST_PEER");
    if let Ok(peer) =
        std::env::var("LIBIPC_TEST_PEER").or_else(|_| std::env::var("DEP_IPC_ECHO_BINARY"))
    {
        println!("cargo:rustc-env=LIBIPC_ECHO_BINARY={peer}");
    }
}
