#ifndef TOMOE_XWM_PROTO_H
#define TOMOE_XWM_PROTO_H

#include <stdint.h>

#define XWM_PROTO_MAGIC 0x58574D31u
#define XWM_PROTO_VERSION 1u
#define XWM_MAX_PAYLOAD 65536u
#define XWM_FLAG_HAS_FD 1u

enum xwm_msg_type {
    XWM_EVT_HELLO = 1,
    XWM_EVT_XWAYLAND_PID = 2,
    XWM_EVT_WINDOW_CREATE = 3,
    XWM_EVT_WINDOW_DESTROY = 4,
    XWM_EVT_WINDOW_PAIR = 5,
    XWM_EVT_WINDOW_MAP = 6,
    XWM_EVT_WINDOW_UNMAP = 7,
    XWM_EVT_WINDOW_GEOMETRY = 8,
    XWM_EVT_WINDOW_META = 9,
    XWM_EVT_WINDOW_STATE_REQ = 10,
    XWM_EVT_SELECTION_OFFER = 11,
    XWM_EVT_SELECTION_CLEAR = 12,
    XWM_EVT_SELECTION_REQUEST = 13,

    XWM_CMD_CONFIGURE = 101,
    XWM_CMD_ACTIVATE = 102,
    XWM_CMD_STATE = 103,
    XWM_CMD_CLOSE = 104,
    XWM_CMD_WAYLAND_SELECTION = 105,
    XWM_CMD_SEND_SELECTION = 106,
};

enum xwm_state_req_kind {
    XWM_STATE_REQ_FULLSCREEN = 0,
    XWM_STATE_REQ_MAXIMIZED = 1,
    XWM_STATE_REQ_ACTIVATE = 2,
    XWM_STATE_REQ_MINIMIZE = 3,
    XWM_STATE_REQ_CLOSE = 4,
};

struct xwm_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t len;
    uint32_t flags;
};

struct xwm_msg_pid {
    int32_t pid;
};

struct xwm_msg_window_create {
    uint32_t xid;
    uint32_t transient;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint8_t override;
    uint8_t dialog;
    uint8_t pad[2];
};

struct xwm_msg_window_id {
    uint32_t xid;
};

struct xwm_msg_window_pair {
    uint32_t xid;
    uint32_t pad;
    uint64_t serial;
};

struct xwm_msg_window_map {
    uint32_t xid;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint8_t override;
    uint8_t dialog;
    uint8_t fullscreen;
    uint8_t maximized;
};

struct xwm_msg_window_geom {
    uint32_t xid;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct xwm_msg_window_meta {
    uint32_t xid;
    uint32_t transient;
    uint32_t title_len;
    uint32_t appid_len;
    uint8_t dialog;
    uint8_t fullscreen;
    uint8_t maximized;
    uint8_t pad;
};

struct xwm_msg_state_req {
    uint32_t xid;
    uint8_t kind;
    uint8_t action;
    uint8_t pad[2];
};

struct xwm_msg_sel_offer {
    uint8_t primary;
    uint8_t pad;
    uint16_t count;
};

struct xwm_msg_sel_clear {
    uint8_t primary;
    uint8_t pad[3];
};

struct xwm_msg_sel_req {
    uint8_t primary;
    uint8_t pad;
    uint16_t mime_len;
};

struct xwm_cmd_configure {
    uint32_t xid;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint8_t has_pos;
    uint8_t has_size;
    uint8_t pad[2];
};

struct xwm_cmd_activate {
    uint32_t xid;
    uint8_t activated;
    uint8_t pad[3];
};

struct xwm_cmd_state {
    uint32_t xid;
    int8_t fullscreen;
    int8_t maximized;
    uint8_t pad[2];
};

struct xwm_cmd_wayland_sel {
    uint8_t primary;
    uint8_t present;
    uint16_t count;
};

#endif
