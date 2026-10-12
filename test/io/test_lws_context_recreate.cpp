// lws itself, with no coro runtime: a client context can be destroyed and another
// created in the same process, as long as one context that did the SSL global init
// stays alive throughout.
//
// coro gives each Runtime its own client context and destroys it with the Runtime.
// lws 4.5.3 to 4.5.8 call OPENSSL_cleanup() when the last context created with
// LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT is destroyed, after which OpenSSL can't be
// used again in the process, so coro keeps an idle "anchor" context for good
// (ensure_ssl_anchor() in src/io/ws_service.cpp). This test is that arrangement in
// lws's own terms. If it fails after an lws upgrade, the anchor no longer does its
// job.

#include <gtest/gtest.h>
#include <libwebsockets.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int protocol_cb(lws*, lws_callback_reasons, void*, void*, size_t) { return 0; }

// lws keeps the pointer, so the array must outlive every context.
const lws_protocols kProtocols[] = {
    {"probe", protocol_cb, 0, 4096, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0},
};

void print_lws_log(int /*level*/, const char* line) { std::fprintf(stderr, "  lws: %s", line); }

// OpenSSL's error queue belongs to the calling thread.
std::string take_openssl_errors() {
    std::string errors;
    while (unsigned long code = ERR_get_error()) {
        char text[256];
        ERR_error_string_n(code, text, sizeof text);
        if (!errors.empty()) errors += "; ";
        errors += text;
    }
    return errors.empty() ? "none queued" : errors;
}

// The first thing to fail once OpenSSL has been cleaned up, and the step of lws's
// client SSL setup that fails without logging: it starts a SHA-256 over the TLS
// configuration (lws_tls_client_create_vhost_context()).
testing::AssertionResult sha256_starts() {
    ERR_clear_error();
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    if (!md)
        return testing::AssertionFailure()
               << "EVP_MD_CTX_new failed; OpenSSL errors: " << take_openssl_errors();
    const int   ok     = EVP_DigestInit_ex(md, EVP_sha256(), nullptr);
    std::string errors = take_openssl_errors();
    EVP_MD_CTX_free(md);
    if (ok != 1)
        return testing::AssertionFailure()
               << "EVP_DigestInit_ex(sha256) returned " << ok << "; OpenSSL errors: " << errors;
    return testing::AssertionSuccess();
}

lws_context* create_context(std::uint64_t options) {
    lws_context_creation_info info{};
    info.port      = CONTEXT_PORT_NO_LISTEN;
    info.protocols = kProtocols;
    info.options   = options;
    return lws_create_context(&info);
}

TEST(LwsContextRecreate, ClientContextsComeAndGoWhileAnchorLives) {
    lws_set_log_level(LLL_ERR | LLL_WARN, print_lws_log);
    std::fprintf(stderr, "  built against lws %s, %s\n", LWS_LIBRARY_VERSION,
                 OPENSSL_VERSION_TEXT);

    // The options coro's anchor uses. Static, volatile and never destroyed, as
    // there: the destroy is what this test is about avoiding.
    static lws_context* volatile anchor = create_context(LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
                                                      LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS);
    ASSERT_NE(static_cast<lws_context*>(anchor), nullptr);

    for (int round = 0; round < 3; ++round) {
        SCOPED_TRACE("round " + std::to_string(round));
        // The options coro's client contexts use.
        lws_context* client = create_context(LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT);
        ASSERT_NE(client, nullptr) << "OpenSSL errors: " << take_openssl_errors();
        lws_context_destroy(client);
        EXPECT_TRUE(sha256_starts());
    }
}

} // namespace
