#include "Notifications.h"

#include <dbus/dbus.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace huskyfe::notifications {
namespace {


constexpr uint32_t REASON_EXPIRED   = 1;
constexpr uint32_t REASON_DISMISSED = 2;
constexpr uint32_t REASON_CLOSE_API = 3;
constexpr uint32_t REASON_UNDEFINED = 4;

constexpr int   MAX_VISIBLE     = 2;
constexpr float TOAST_W_PX      = 1100.0f;
constexpr float TOAST_H_PX      = 200.0f;
constexpr float TOAST_GAP_PX    = 18.0f;
constexpr float TOAST_TOP_PX    = 110.0f;
constexpr float TOAST_RADIUS    = 36.0f;
constexpr float DEFAULT_TIMEOUT_MS = 5000.0f;
constexpr float EXIT_ANIM_S     = 0.30f;


struct Toast {
    uint32_t    id        = 0;
    std::string app_name;
    std::string summary;
    std::string body;
    std::string app_icon;
    int32_t     urgency   = 1;
    float       lifetime_s = DEFAULT_TIMEOUT_MS / 1000.0f;
    float       age_s     = 0.0f;
    Spring      slide;
    bool        exiting   = false;
    float       exit_age  = 0.0f;

    float       y_target  = TOAST_TOP_PX;
    Spring      y_anim;
};

struct State {
    int                screen_w = 0;
    int                screen_h = 0;

    std::mutex         mu;

    struct Incoming {
        bool        is_close;
        uint32_t    close_id;

        Toast       t;
        bool        is_replacement;
    };
    std::vector<Incoming> inbox;


    struct ClosedOut { uint32_t id; uint32_t reason; };
    std::vector<ClosedOut> closed_out;


    std::vector<Toast> active;
    uint32_t           next_id_main = 1;


    int  pressed_idx = -1;


    std::thread        worker;
    std::atomic<bool>  running{false};
    std::atomic<bool>  muted{false};
    DBusConnection*    conn = nullptr;


    std::vector<HistoryEntry> history;
};

constexpr size_t HISTORY_CAP = 50;

static int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

State g;

TapHandler g_tap_handler;


const char* INTROSPECT_XML =
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
"<node>\n"
"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
"    <method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method>\n"
"  </interface>\n"
"  <interface name=\"org.freedesktop.Notifications\">\n"
"    <method name=\"GetCapabilities\">\n"
"      <arg type=\"as\" direction=\"out\"/>\n"
"    </method>\n"
"    <method name=\"Notify\">\n"
"      <arg name=\"app_name\"        type=\"s\"     direction=\"in\"/>\n"
"      <arg name=\"replaces_id\"     type=\"u\"     direction=\"in\"/>\n"
"      <arg name=\"app_icon\"        type=\"s\"     direction=\"in\"/>\n"
"      <arg name=\"summary\"         type=\"s\"     direction=\"in\"/>\n"
"      <arg name=\"body\"            type=\"s\"     direction=\"in\"/>\n"
"      <arg name=\"actions\"         type=\"as\"    direction=\"in\"/>\n"
"      <arg name=\"hints\"           type=\"a{sv}\" direction=\"in\"/>\n"
"      <arg name=\"expire_timeout\"  type=\"i\"     direction=\"in\"/>\n"
"      <arg type=\"u\" direction=\"out\"/>\n"
"    </method>\n"
"    <method name=\"CloseNotification\">\n"
"      <arg type=\"u\" direction=\"in\"/>\n"
"    </method>\n"
"    <method name=\"GetServerInformation\">\n"
"      <arg type=\"s\" direction=\"out\"/>\n"
"      <arg type=\"s\" direction=\"out\"/>\n"
"      <arg type=\"s\" direction=\"out\"/>\n"
"      <arg type=\"s\" direction=\"out\"/>\n"
"    </method>\n"
"    <signal name=\"NotificationClosed\">\n"
"      <arg type=\"u\"/><arg type=\"u\"/>\n"
"    </signal>\n"
"    <signal name=\"ActionInvoked\">\n"
"      <arg type=\"u\"/><arg type=\"s\"/>\n"
"    </signal>\n"
"  </interface>\n"
"</node>\n";


bool read_byte_or_u32(DBusMessageIter* var, int32_t* out) {
    int t = dbus_message_iter_get_arg_type(var);
    if (t == DBUS_TYPE_BYTE)   { uint8_t  v = 0; dbus_message_iter_get_basic(var, &v); *out = v; return true; }
    if (t == DBUS_TYPE_UINT32) { uint32_t v = 0; dbus_message_iter_get_basic(var, &v); *out = (int32_t)v; return true; }
    if (t == DBUS_TYPE_INT32)  { int32_t  v = 0; dbus_message_iter_get_basic(var, &v); *out = v; return true; }
    return false;
}


bool parse_notify(DBusMessage* msg, Toast& out, uint32_t& replaces_id, int32_t& expire_ms) {
    DBusMessageIter it;
    if (!dbus_message_iter_init(msg, &it)) return false;

    auto take_string = [&](std::string& s) {
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return false;
        const char* v = nullptr;
        dbus_message_iter_get_basic(&it, &v);
        s = v ? v : "";
        dbus_message_iter_next(&it);
        return true;
    };
    auto take_u32 = [&](uint32_t& v) {
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return false;
        dbus_message_iter_get_basic(&it, &v);
        dbus_message_iter_next(&it);
        return true;
    };
    auto take_i32 = [&](int32_t& v) {
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INT32) return false;
        dbus_message_iter_get_basic(&it, &v);
        dbus_message_iter_next(&it);
        return true;
    };

    if (!take_string(out.app_name))     return false;
    if (!take_u32   (replaces_id))      return false;
    if (!take_string(out.app_icon))     return false;
    if (!take_string(out.summary))      return false;
    if (!take_string(out.body))         return false;


    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return false;
    dbus_message_iter_next(&it);


    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        DBusMessageIter dict;
        dbus_message_iter_recurse(&it, &dict);
        while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse(&dict, &entry);
            const char* key = nullptr;
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&entry, &key);
            }
            dbus_message_iter_next(&entry);
            if (key && std::strcmp(key, "urgency") == 0
                && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                DBusMessageIter var;
                dbus_message_iter_recurse(&entry, &var);
                read_byte_or_u32(&var, &out.urgency);
            }
            dbus_message_iter_next(&dict);
        }
        dbus_message_iter_next(&it);
    }

    if (!take_i32(expire_ms)) return false;
    return true;
}

void send_uint32_reply(DBusMessage* msg, uint32_t value) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    DBusMessageIter it;
    dbus_message_iter_init_append(reply, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &value);
    dbus_connection_send(g.conn, reply, nullptr);
    dbus_message_unref(reply);
}

void send_introspect_reply(DBusMessage* msg) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    const char* xml = INTROSPECT_XML;
    DBusMessageIter it;
    dbus_message_iter_init_append(reply, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &xml);
    dbus_connection_send(g.conn, reply, nullptr);
    dbus_message_unref(reply);
}

void emit_notification_closed(uint32_t id, uint32_t reason) {
    DBusMessage* sig = dbus_message_new_signal(
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "NotificationClosed");
    if (!sig) return;
    DBusMessageIter it;
    dbus_message_iter_init_append(sig, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &id);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &reason);
    dbus_connection_send(g.conn, sig, nullptr);
    dbus_message_unref(sig);
}

DBusHandlerResult on_message(DBusConnection* , DBusMessage* msg, void* ) {
    const char* iface  = dbus_message_get_interface(msg);
    const char* member = dbus_message_get_member(msg);
    if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (std::strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0
        && std::strcmp(member, "Introspect") == 0) {
        send_introspect_reply(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (std::strcmp(iface, "org.freedesktop.Notifications") != 0) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (std::strcmp(member, "GetCapabilities") == 0) {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        DBusMessageIter it, arr;
        dbus_message_iter_init_append(reply, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);


        const char* caps[] = { "body" };
        for (const char* c : caps) {
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &c);
        }
        dbus_message_iter_close_container(&it, &arr);
        dbus_connection_send(g.conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (std::strcmp(member, "GetServerInformation") == 0) {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        DBusMessageIter it;
        dbus_message_iter_init_append(reply, &it);
        const char* name = "huskyfe";
        const char* vendor = "huskyfe";
        const char* version = "0.1";
        const char* spec = "1.2";
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &vendor);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &version);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &spec);
        dbus_connection_send(g.conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (std::strcmp(member, "CloseNotification") == 0) {
        DBusMessageIter it;
        if (!dbus_message_iter_init(msg, &it)
            || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
            DBusMessage* err = dbus_message_new_error(msg,
                DBUS_ERROR_INVALID_ARGS, "expected u");
            dbus_connection_send(g.conn, err, nullptr);
            dbus_message_unref(err);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        uint32_t id = 0;
        dbus_message_iter_get_basic(&it, &id);
        {
            std::lock_guard<std::mutex> lk(g.mu);
            State::Incoming in{};
            in.is_close = true;
            in.close_id = id;
            g.inbox.push_back(std::move(in));
        }
        DBusMessage* reply = dbus_message_new_method_return(msg);
        dbus_connection_send(g.conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (std::strcmp(member, "Notify") == 0) {
        Toast t;
        uint32_t replaces_id = 0;
        int32_t  expire_ms   = -1;
        if (!parse_notify(msg, t, replaces_id, expire_ms)) {
            DBusMessage* err = dbus_message_new_error(msg,
                DBUS_ERROR_INVALID_ARGS, "Notify: malformed args");
            dbus_connection_send(g.conn, err, nullptr);
            dbus_message_unref(err);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (expire_ms < 0) t.lifetime_s = DEFAULT_TIMEOUT_MS / 1000.0f;
        else if (expire_ms == 0) t.lifetime_s = 1.0e9f;
        else t.lifetime_s = (float)expire_ms / 1000.0f;


        static std::atomic<uint32_t> id_seq{1};
        uint32_t id = (replaces_id != 0) ? replaces_id : id_seq.fetch_add(1);
        t.id = id;
        t.slide.value = 0.0f; t.slide.target = 1.0f;
        t.slide.stiffness = 240.0f; t.slide.damping = 22.0f;
        t.y_anim.snap_to(TOAST_TOP_PX);


        HistoryEntry he{};
        he.id         = id;
        he.app_name   = t.app_name;
        he.summary    = t.summary;
        he.body       = t.body;
        he.urgency    = t.urgency;
        he.ts_unix_ms = now_unix_ms();


        if (g.muted.load(std::memory_order_acquire) && t.urgency < 2) {
            std::lock_guard<std::mutex> lk(g.mu);

            if (replaces_id != 0) {
                g.history.erase(std::remove_if(g.history.begin(), g.history.end(),
                    [&](const HistoryEntry& h){ return h.id == replaces_id; }),
                    g.history.end());
            }
            g.history.push_back(std::move(he));
            if (g.history.size() > HISTORY_CAP) g.history.erase(g.history.begin());
            g.closed_out.push_back({ id, REASON_DISMISSED });
            send_uint32_reply(msg, id);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        {
            std::lock_guard<std::mutex> lk(g.mu);
            if (replaces_id != 0) {
                g.history.erase(std::remove_if(g.history.begin(), g.history.end(),
                    [&](const HistoryEntry& h){ return h.id == replaces_id; }),
                    g.history.end());
            }
            g.history.push_back(std::move(he));
            if (g.history.size() > HISTORY_CAP) g.history.erase(g.history.begin());

            State::Incoming in{};
            in.is_close = false;
            in.t = std::move(t);
            in.is_replacement = (replaces_id != 0);
            g.inbox.push_back(std::move(in));
        }
        send_uint32_reply(msg, id);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void worker_main() {
    DBusError err;
    dbus_error_init(&err);

    g.conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "notifications: session bus connect failed: %s\n", err.message);
        dbus_error_free(&err);
        return;
    }
    if (!g.conn) {
        fprintf(stderr, "notifications: session bus not available\n");
        return;
    }
    dbus_connection_set_exit_on_disconnect(g.conn, FALSE);

    int rc = dbus_bus_request_name(g.conn,
        "org.freedesktop.Notifications",
        DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE,
        &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "notifications: RequestName failed: %s\n", err.message);
        dbus_error_free(&err);
        dbus_connection_close(g.conn);
        dbus_connection_unref(g.conn);
        g.conn = nullptr;
        return;
    }
    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
        && rc != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        fprintf(stderr, "notifications: another notification daemon owns the name (rc=%d)\n", rc);
        dbus_connection_close(g.conn);
        dbus_connection_unref(g.conn);
        g.conn = nullptr;
        return;
    }

    DBusObjectPathVTable vt{};
    vt.message_function = on_message;
    if (!dbus_connection_register_object_path(g.conn,
        "/org/freedesktop/Notifications", &vt, nullptr)) {
        fprintf(stderr, "notifications: register_object_path failed\n");
        dbus_connection_close(g.conn);
        dbus_connection_unref(g.conn);
        g.conn = nullptr;
        return;
    }

    fprintf(stderr, "notifications: serving org.freedesktop.Notifications on session bus\n");

    while (g.running.load(std::memory_order_acquire)) {

        std::vector<State::ClosedOut> outs;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            outs.swap(g.closed_out);
        }
        for (auto& c : outs) emit_notification_closed(c.id, c.reason);


        if (!dbus_connection_read_write_dispatch(g.conn, 100)) break;
    }

    dbus_connection_close(g.conn);
    dbus_connection_unref(g.conn);
    g.conn = nullptr;
}


void close_active(size_t idx, uint32_t reason) {
    if (idx >= g.active.size()) return;
    Toast& t = g.active[idx];
    if (!t.exiting) {
        t.exiting = true;
        t.exit_age = 0.0f;
        t.slide.set(0.0f);
        std::lock_guard<std::mutex> lk(g.mu);
        g.closed_out.push_back({ t.id, reason });
    }
}


int find_active(uint32_t id) {
    for (size_t i = 0; i < g.active.size(); i++) {
        if (g.active[i].id == id) return (int)i;
    }
    return -1;
}

void layout_y_targets() {


    int slot = 0;
    for (size_t i = 0; i < g.active.size(); i++) {
        Toast& t = g.active[i];
        if (t.exiting) continue;
        if (slot < MAX_VISIBLE) {
            t.y_target = TOAST_TOP_PX + slot * (TOAST_H_PX + TOAST_GAP_PX);
            t.y_anim.set(t.y_target);

            t.slide.set(1.0f);
            slot++;
        } else {

            t.y_target = -TOAST_H_PX - 40.0f;
            t.y_anim.set(t.y_target);
            t.slide.snap_to(0.0f);
        }
    }


}

}


bool init(int screen_w, int screen_h) {
    g.screen_w = screen_w;
    g.screen_h = screen_h;
    if (g.running.load()) return true;
    g.running.store(true, std::memory_order_release);
    g.worker = std::thread(worker_main);
    return true;
}

void shutdown() {
    if (!g.running.exchange(false)) return;


    if (g.worker.joinable()) g.worker.join();
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.inbox.clear();
        g.closed_out.clear();
    }
    g.active.clear();
}

void tick(float dt) {

    std::vector<State::Incoming> drained;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        drained.swap(g.inbox);
    }
    for (auto& in : drained) {
        if (in.is_close) {
            int idx = find_active(in.close_id);
            if (idx >= 0) close_active((size_t)idx, REASON_CLOSE_API);
            continue;
        }


        if (in.is_replacement) {
            int idx = find_active(in.t.id);
            if (idx >= 0) {
                Toast& cur = g.active[idx];
                cur.app_name  = in.t.app_name;
                cur.summary   = in.t.summary;
                cur.body      = in.t.body;
                cur.app_icon  = in.t.app_icon;
                cur.urgency   = in.t.urgency;
                cur.lifetime_s= in.t.lifetime_s;
                cur.age_s     = 0.0f;
                cur.exiting   = false;
                cur.exit_age  = 0.0f;
                cur.slide.set(1.0f);
                continue;
            }
        }
        g.active.push_back(std::move(in.t));
    }


    layout_y_targets();


    for (auto& t : g.active) {
        t.slide.tick(dt);
        t.y_anim.tick(dt);
        if (t.exiting) {
            t.exit_age += dt;
        } else {
            t.age_s += dt;
            if (t.age_s >= t.lifetime_s) {
                t.exiting = true;
                t.slide.set(0.0f);
                std::lock_guard<std::mutex> lk(g.mu);
                g.closed_out.push_back({ t.id, REASON_EXPIRED });
            }
        }
    }


    g.active.erase(std::remove_if(g.active.begin(), g.active.end(),
        [](const Toast& t) {
            return t.exiting && t.exit_age >= EXIT_ANIM_S && t.slide.value < 0.02f;
        }), g.active.end());
}

bool any_visible() {
    for (auto& t : g.active) {
        if (!t.exiting && t.slide.value > 0.02f) return true;
    }
    return false;
}

void set_muted(bool muted) {
    g.muted.store(muted, std::memory_order_release);
    if (muted) {


        for (size_t i = 0; i < g.active.size(); i++) {
            if (g.active[i].urgency < 2) close_active(i, REASON_DISMISSED);
        }
    }
}

bool is_muted() {
    return g.muted.load(std::memory_order_acquire);
}

std::vector<HistoryEntry> history_snapshot() {
    std::vector<HistoryEntry> out;
    std::lock_guard<std::mutex> lk(g.mu);
    out.reserve(g.history.size());

    for (auto it = g.history.rbegin(); it != g.history.rend(); ++it) {
        out.push_back(*it);
    }
    return out;
}

void clear_one(uint32_t id) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.history.erase(std::remove_if(g.history.begin(), g.history.end(),
        [&](const HistoryEntry& h){ return h.id == id; }), g.history.end());
}

void clear_low() {
    std::lock_guard<std::mutex> lk(g.mu);
    g.history.erase(std::remove_if(g.history.begin(), g.history.end(),
        [](const HistoryEntry& h){ return h.urgency <= 0; }), g.history.end());
}

void clear_all() {
    std::lock_guard<std::mutex> lk(g.mu);
    g.history.clear();
}

static Color color_accent_for(int urgency) {
    if (urgency <= 0) return { 0.40f, 0.55f, 0.95f, 1.0f };
    if (urgency >= 2) return { 0.95f, 0.30f, 0.30f, 1.0f };
    return                      { 0.30f, 0.80f, 0.50f, 1.0f };
}


static void draw_severity_badge(Renderer& r, float cx, float cy,
                                int urgency, float alpha) {
    constexpr float D = 64.0f;
    const Color base = color_accent_for(urgency);
    Color fill{ base.r, base.g, base.b, alpha };
    Color bot { base.r * 0.55f, base.g * 0.55f, base.b * 0.55f, alpha };

    r.draw_rect_gradient(cx - D * 0.5f, cy - D * 0.5f, D, D,
                         fill, bot, D * 0.5f);

    const Color ink{ 1.0f, 1.0f, 1.0f, alpha };

    if (urgency <= 0) {

        const float bw = 7.0f;
        const float bh = 22.0f;
        const float dot = 8.0f;
        const float gap = 5.0f;
        const float total_h = dot + gap + bh;
        const float top_y = cy - total_h * 0.5f;

        r.draw_rect(cx - dot * 0.5f, top_y, dot, dot, ink, dot * 0.5f);

        r.draw_rect(cx - bw * 0.5f, top_y + dot + gap, bw, bh, ink, bw * 0.5f);
    } else if (urgency >= 2) {

        const float bw = 8.0f;
        const float bh = 24.0f;
        const float dot = 8.0f;
        const float gap = 5.0f;
        const float total_h = bh + gap + dot;
        const float top_y = cy - total_h * 0.5f;
        r.draw_rect(cx - bw * 0.5f, top_y, bw, bh, ink, bw * 0.5f);
        r.draw_rect(cx - dot * 0.5f, top_y + bh + gap, dot, dot, ink, dot * 0.5f);
    } else {


        const float body_w = 30.0f;
        const float body_h = 26.0f;
        const float rim_w  = 36.0f;
        const float rim_h  = 5.0f;
        const float clp_d  = 8.0f;
        const float stem_w = 5.0f;
        const float stem_h = 4.0f;
        const float total_h = stem_h + body_h + rim_h + clp_d + 1.0f;
        const float top_y = cy - total_h * 0.5f;

        r.draw_rect(cx - stem_w * 0.5f, top_y, stem_w, stem_h,
                    ink, stem_w * 0.5f);


        r.draw_rect(cx - body_w * 0.5f, top_y + stem_h, body_w, body_h,
                    ink, body_w * 0.42f);

        r.draw_rect(cx - rim_w * 0.5f, top_y + stem_h + body_h, rim_w, rim_h,
                    ink, rim_h * 0.5f);

        r.draw_rect(cx - clp_d * 0.5f,
                    top_y + stem_h + body_h + rim_h + 1.0f,
                    clp_d, clp_d, ink, clp_d * 0.5f);
    }
}

void render(Renderer& r, ImageRenderer& , TextRenderer& title,
            TextRenderer& body) {
    if (g.active.empty()) return;
    const float sw = (float)g.screen_w;


    for (const auto& t : g.active) {
        float a = std::clamp(t.slide.value, 0.0f, 1.0f);
        if (a <= 0.005f) continue;

        float slide_off = (1.0f - a) * (TOAST_H_PX + 40.0f);
        float y = t.y_anim.value - slide_off;

        if (y + TOAST_H_PX < 0.0f) continue;
        float x = (sw - TOAST_W_PX) * 0.5f;


        r.draw_rect(x, y + 6.0f, TOAST_W_PX, TOAST_H_PX,
                    { 0.0f, 0.0f, 0.0f, 0.30f * a }, TOAST_RADIUS);

        Color top_c{ 0.16f, 0.17f, 0.21f, 0.97f * a };
        Color bot_c{ 0.10f, 0.11f, 0.14f, 0.97f * a };
        r.draw_rect_gradient(x, y, TOAST_W_PX, TOAST_H_PX, top_c, bot_c, TOAST_RADIUS);


        const float badge_cx = x + 60.0f;
        const float badge_cy = y + TOAST_H_PX * 0.5f;
        draw_severity_badge(r, badge_cx, badge_cy, t.urgency, 0.95f * a);
    }
    r.flush();


    title.begin(r.xform_data());
    for (const auto& t : g.active) {
        float a = std::clamp(t.slide.value, 0.0f, 1.0f);
        if (a <= 0.005f) continue;
        float slide_off = (1.0f - a) * (TOAST_H_PX + 40.0f);
        float y = t.y_anim.value - slide_off;
        if (y + TOAST_H_PX < 0.0f) continue;
        float x = (sw - TOAST_W_PX) * 0.5f;

        Color fg{ 1.0f, 1.0f, 1.0f, 0.98f * a };
        Color dim{ 0.78f, 0.80f, 0.86f, 0.92f * a };


        constexpr float TEXT_LEFT_PAD = 120.0f;
        const float card_cx = x + TEXT_LEFT_PAD
                            + (TOAST_W_PX - TEXT_LEFT_PAD - 30.0f) * 0.5f;
        if (!t.app_name.empty()) {
            constexpr float app_scale = 0.55f;
            float aw = title.measure_width(t.app_name.c_str()) * app_scale;
            float app_y = y + 36.0f + title.ascent() * 0.32f * app_scale;
            title.draw(card_cx - aw * 0.5f, app_y,
                       t.app_name.c_str(), dim, app_scale);
        }
        if (!t.summary.empty()) {
            constexpr float sum_scale = 0.85f;
            float sw_text = title.measure_width(t.summary.c_str()) * sum_scale;


            float sum_y = y + TOAST_H_PX * 0.5f + title.ascent() * 0.32f * sum_scale;
            title.draw(card_cx - sw_text * 0.5f, sum_y,
                       t.summary.c_str(), fg, sum_scale);
        }
    }
    title.end();


    body.begin(r.xform_data());
    for (const auto& t : g.active) {
        float a = std::clamp(t.slide.value, 0.0f, 1.0f);
        if (a <= 0.005f) continue;
        if (t.body.empty()) continue;
        float slide_off = (1.0f - a) * (TOAST_H_PX + 40.0f);
        float y = t.y_anim.value - slide_off;
        if (y + TOAST_H_PX < 0.0f) continue;
        float x = (sw - TOAST_W_PX) * 0.5f;
        Color body_c{ 0.86f, 0.88f, 0.92f, 0.93f * a };


        const float max_w = TOAST_W_PX - 80.0f;
        std::string line = t.body;

        for (auto& c : line) if (c == '\n' || c == '\r') c = ' ';

        while (body.measure_width(line.c_str()) > max_w && line.size() > 1) {
            line.pop_back();
        }
        if (line.size() < t.body.size()) {

            while (line.size() > 3 && body.measure_width((line + "...").c_str()) > max_w) {
                line.pop_back();
            }
            line += "...";
        }
        float by = y + TOAST_H_PX - 38.0f;
        float lw = body.measure_width(line.c_str());
        constexpr float TEXT_LEFT_PAD = 120.0f;
        float card_cx = x + TEXT_LEFT_PAD
                      + (TOAST_W_PX - TEXT_LEFT_PAD - 30.0f) * 0.5f;
        body.draw(card_cx - lw * 0.5f, by, line.c_str(), body_c);
    }
    body.end();
}

bool on_touch_down(int x, int y) {
    g.pressed_idx = -1;
    if (g.active.empty()) return false;
    const float sw = (float)g.screen_w;
    const float xL = (sw - TOAST_W_PX) * 0.5f;
    const float xR = xL + TOAST_W_PX;
    if (x < xL || x > xR) return false;
    for (size_t i = 0; i < g.active.size(); i++) {
        const Toast& t = g.active[i];
        float a = std::clamp(t.slide.value, 0.0f, 1.0f);
        if (a <= 0.2f) continue;
        if (t.exiting) continue;
        float slide_off = (1.0f - a) * (TOAST_H_PX + 40.0f);
        float yT = t.y_anim.value - slide_off;
        float yB = yT + TOAST_H_PX;
        if ((float)y >= yT && (float)y <= yB) {
            g.pressed_idx = (int)i;
            return true;
        }
    }
    return false;
}

bool on_touch_up(int x, int y) {
    if (g.pressed_idx < 0) return false;
    int idx = g.pressed_idx;
    g.pressed_idx = -1;
    if ((size_t)idx >= g.active.size()) return false;
    const float sw = (float)g.screen_w;
    const float xL = (sw - TOAST_W_PX) * 0.5f;
    const float xR = xL + TOAST_W_PX;
    const Toast& t = g.active[idx];
    float a = std::clamp(t.slide.value, 0.0f, 1.0f);
    float slide_off = (1.0f - a) * (TOAST_H_PX + 40.0f);
    float yT = t.y_anim.value - slide_off;
    float yB = yT + TOAST_H_PX;
    if ((float)x >= xL && (float)x <= xR && (float)y >= yT && (float)y <= yB) {
        uint32_t    tap_id   = t.id;
        std::string tap_app  = t.app_name;
        close_active((size_t)idx, REASON_DISMISSED);
        if (g_tap_handler) g_tap_handler(tap_id, tap_app);
        return true;
    }
    return false;
}

void set_tap_handler(TapHandler h) {
    g_tap_handler = std::move(h);
}

uint32_t post_local(const std::string& app_name,
                    const std::string& summary,
                    const std::string& body,
                    int32_t            expire_ms) {
    std::lock_guard<std::mutex> lk(g.mu);
    State::Incoming in{};
    in.is_close = false;
    in.is_replacement = false;
    in.t.id        = g.next_id_main++;
    in.t.app_name  = app_name;
    in.t.summary   = summary;
    in.t.body      = body;
    in.t.urgency   = 1;
    in.t.lifetime_s = (expire_ms > 0) ? (expire_ms / 1000.0f)
                                       : (DEFAULT_TIMEOUT_MS / 1000.0f);
    uint32_t id = in.t.id;
    g.inbox.push_back(std::move(in));
    return id;
}

}
