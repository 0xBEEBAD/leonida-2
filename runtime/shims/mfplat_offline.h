// Offline Media Foundation (mfplat.dll) source resolver proxy.
//
// Keeps Media Foundation local-only: host mfplat.dll handles codecs and playback, while MFCreateSourceResolver wraps
// the host IMFSourceResolver so network URLs (http/https/rtsp/mms, UNC shares, remote file:// hosts) are refused with
// MF_E_UNSUPPORTED_SCHEME and console paths (GAME:, G:, T:) are rewritten through rt::fs::resolve_read_path.
#pragma once

#include <windows.h>
#include <unknwn.h>

#include <string>

namespace rt::mf {

// MF_E_UNSUPPORTED_SCHEME (mferror.h)
inline constexpr HRESULT kUnsupportedScheme = static_cast<HRESULT>(0xC00D36C3u);

// IID_IMFSourceResolver ({FBE5A32D-A497-4B61-BB85-97B1A848A6E3}, mfidl.h)
inline constexpr GUID kIidSourceResolver = {
    0xFBE5A32Du, 0xA497u, 0x4B61u, {0xBBu, 0x85u, 0x97u, 0xB1u, 0xA8u, 0x48u, 0xA6u, 0xE3u}};

// Validates a Media Foundation URL and resolves console drives (GAME:, G:, T:) to host paths for reading.
// Returns S_OK with `out` populated for local paths and local file:// URLs, E_POINTER for null, kUnsupportedScheme
// for network schemes / UNC paths / non-local file:// hosts (filling `refused_scheme` and `refused_host` if given),
// or a Win32 HRESULT if a console path is rejected by rt::fs::resolve_read_path.
HRESULT resolve_media_url(const wchar_t* url, std::wstring& out, std::string* refused_scheme = nullptr,
                          std::string* refused_host = nullptr);

// Wraps an existing host IMFSourceResolver COM pointer in the offline proxy. Takes ownership of one reference on
// `host_resolver` on success; on failure the caller retains its reference and `*out` is nulled.
HRESULT wrap_source_resolver(IUnknown* host_resolver, void** out);

}  // namespace rt::mf
