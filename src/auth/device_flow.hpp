#pragma once

#include "auth/session.hpp"
#include "platform/credential_store.hpp"

namespace astral::auth {

// Human-facing progress: hands out the verification URL and the user code to
// type in. The command layer opens a browser and/or prints them.
using OnUserCodeFn =
    std::function<void(const std::string& verificationUrl, const std::string& userCode)>;

// Runs the RFC 8628 device-code flow against `serverUrl` and returns a
// complete human session (modulator TODO §11 D12).
//
// Flow (protocol snapshot v1, A1):
//   GET  {origin}/.well-known/astral                      -> server_id/api_base
//   POST {api}/auth/device/authorizations                 -> 201 codes
//   (onUserCode)                                          -> human approves
//   POST {api}/auth/device/authorizations/{code}/token    -> poll until 200
//
// Throws core::AstralError:
//   SERVER_NOT_FOUND       well-known missing/not an astral server
//   PROTOCOL_INCOMPATIBLE  unexpected shapes, protocol version mismatch
//   AUTH_REQUIRED          approval denied / device code expired before use
//   TIMEOUT                deadline passed while still pending
platform::LoginSession runDeviceFlow(const std::string& serverUrl, const HttpFn& http,
                                     const SleepFn& sleep, const OnUserCodeFn& onUserCode);

} // namespace astral::auth
