# libwebsockets 4.3.3, lws_http_client_socket_service(): when the HTTP upgrade
# request fails to write it calls lws_close_free_wsi() and then returns 0,
# which its caller rops_handle_POLLIN_h1() reads as "still alive" and goes on
# to dereference the freed wsi (ops-h1.c:704). Every other close in that
# function returns non-zero, which the caller maps to
# LWS_HPI_RET_WSI_ALREADY_DIED.
#
# A TLS 1.3 server that rejects our client certificate produces exactly that
# write failure: its alert arrives only once we write, so the reject path is a
# remotely reachable use-after-free. Covered by
# WsTls.MutualTlsFailsWithoutAClientCertificate, which aborts under ASan
# without this.
#
# String replacement rather than patch(1): it needs no extra tool in the Alpine
# build image and re-running it on an already-patched tree is a no-op.

set(target "${LWS_SOURCE_DIR}/lib/roles/http/client/client-http.c")
if(NOT EXISTS "${target}")
  message(FATAL_ERROR "lws patch: ${target} does not exist")
endif()

file(READ "${target}" content)

set(site "lws_close_free_wsi\\(wsi, LWS_CLOSE_STATUS_NOSTATUS,[ \t\r\n]*\"cws\"\\)\\;[ \t\r\n]*return ")

if(content MATCHES "${site}-1\\;")
  return()
endif()

if(NOT content MATCHES "${site}0\\;")
  message(FATAL_ERROR "lws patch: the use-after-free site changed; re-verify it "
                      "against the new libwebsockets before dropping this patch")
endif()

string(REGEX REPLACE "(${site})0\\;" "\\1-1;" content "${content}")
file(WRITE "${target}" "${content}")
message(STATUS "libwebsockets: patched the client-http use-after-free on write failure")
