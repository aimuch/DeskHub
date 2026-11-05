// Dev-WifiList / Dev-Keyboard / Dev-WifiResult. Widgets live on the LVGL task;
// radio work is queued to wifi_change. Rebuilds happen on ticks, never inside a release event.
#include "ui_internal.hpp"
#include "ui_keyboard.hpp"
#include "wifi_change.hpp"
#include <algorithm>
#include <cstdio>

namespace dh::ui::detail {
namespace {
namespace wc = dh::wifi_change;
enum class View { List, Name, Security, Password, Result };
View view = View::List;
Keyboard editor;
lv_obj_t *root = nullptr, *input = nullptr, *count = nullptr, *hint = nullptr, *done = nullptr, *eye = nullptr;
lv_obj_t *result_title = nullptr, *result_note = nullptr, *remaining = nullptr;
bool opened = false, dirty = false, closing = false, selected_open = false, refused = false, last_restoring = false;
int key_page = 0;
std::string ssid;
int64_t last_touch = 0, delete_due = 0;
wc::Phase last_phase = wc::Phase::Idle;
wc::Reason last_reason = wc::Reason::None;
const Palette &P() { return look.palette; }
const char *txt(const char *zh, const char *en) { return look.lang == Lang::EnUS ? en : zh; }
void wipe(std::string &s) { if (!s.empty()) { volatile char *p = &s[0]; for (size_t i=0;i<s.size();++i) p[i]=0; } s.clear(); }
void wipe_label() {
    if (!input) return;
    // LVGL owns a copy of the revealed password. Erase that buffer before replacing it.
    char *p = const_cast<char *>(lv_label_get_text(input));
    if (p) { volatile char *v=p; for (size_t i=0;p[i];++i) v[i]=0; }
}
void redact() { editor.reveal(false); wipe_label(); if (input) set_text(input, editor.display()); }
void activity() { note_activity(); wc::touch(); last_touch = monotonic_ms(); }
lv_obj_t *text_at(lv_obj_t *parent,const std::string &s,int x,int y,int w,const lv_font_t *font,uint32_t color) {
    return label_at(parent,s,x,y,w,font,color);
}
void center(lv_obj_t *o) { lv_obj_center(o); lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0); }
lv_obj_t *action(lv_obj_t *parent,const std::string &s,int x,int y,int w,int h,lv_event_cb_t cb,int id=0,bool symbol=false) {
    auto *b=button(parent,cb,reinterpret_cast<void *>(static_cast<intptr_t>(id)),w,h);
    lv_obj_set_pos(b,x,y);
    lv_obj_remove_flag(b,LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_radius(b,14,0);
    auto *l=label(b,s,w-8,symbol ? &lv_font_montserrat_24 : &btr_ui_20,P().ink);
    center(l); return b;
}
int event_id(lv_event_t *e) { return static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))); }
void refresh_input() {
    if (!input) return;
    wipe_label();
    std::string rendered=editor.display(); set_text(input,rendered); wipe(rendered);
    char n[24]; snprintf(n,sizeof(n),"%u/%u",static_cast<unsigned>(editor.size()),static_cast<unsigned>(editor.limit()));
    set_text(count,n);
    // What is wrong now, or else the rule. The count beside the field says how far it is.
    char need[48]; snprintf(need,sizeof(need),txt("还差 %u 个字符","%u more characters"),static_cast<unsigned>(editor.missing()));
    const bool short_now=editor.size()>0 && editor.missing()>0;
    set_text(hint,editor.full() ? (editor.password() ? (editor.size()==64 ? txt("64 位十六进制密码","64 hex digit key") : txt("最多 63 个字符","63 characters at most")) : txt("最多 32 个字节","32 bytes at most"))
                  : short_now ? need : editor.password() ? txt("8 到 63 个字符","8 to 63 characters") : txt("名称不遮住；1 到 32 个字节","Name is visible; 1-32 bytes"));
    set_ink(hint,short_now || (editor.full() && editor.size()!=64) ? P().power : P().muted);
    if (editor.valid()) lv_obj_remove_state(done,LV_STATE_DISABLED); else lv_obj_add_state(done,LV_STATE_DISABLED);
    lv_obj_set_style_opa(done,editor.valid() ? LV_OPA_COVER : LV_OPA_40,0);
}
void key_event(lv_event_t *e) {
    const int id=event_id(e); activity(); redact();
    if (id==256) { key_page=key_page==1 ? 0 : 1; dirty=true; }
    else if (id==257) { key_page=key_page>=2 ? 0 : 2; dirty=true; }
    else if (id==258) { key_page=key_page==2 ? 3 : 2; dirty=true; }
    else if (id==259) editor.erase();
    else editor.append(static_cast<char>(id));
    refresh_input();
}
void delete_event(lv_event_t *e) {
    auto code=lv_event_get_code(e);
    if(code==LV_EVENT_PRESSED) { activity(); redact(); delete_due=monotonic_ms()+500; }
    if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST) delete_due=0;
}
void reveal_event(lv_event_t *e) {
    const auto code=lv_event_get_code(e);
    if(code==LV_EVENT_PRESSED) { activity(); editor.reveal(true); refresh_input(); }
    if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST || code==LV_EVENT_DEFOCUSED) redact();
}
void submit_editor(lv_event_t *) {
    activity(); redact(); if(!editor.valid()) return;
    if(view==View::Name) { ssid=editor.value(); editor.clear(); view=View::Security; dirty=true; return; }
    auto password=editor.value();
    const bool accepted=wc::begin(ssid,password,false);
    wipe(password);
    if(accepted) { editor.clear(); wipe_label(); view=View::Result; dirty=true; }
    else { set_text(hint,txt("现在不能连接，请稍等","Not ready to connect. Wait a moment.")); set_ink(hint,P().power); }
}
void security_event(lv_event_t *e) {
    activity();
    selected_open = event_id(e)==0;
    if(!selected_open) { editor.reset(true); key_page=0; view=View::Password; dirty=true; }
    else if(wc::begin(ssid,"",true)) { view=View::Result; dirty=true; }
}
void choose_network(lv_event_t *e) {
    activity(); refused=false;
    const auto nets=wc::networks(); const int i=event_id(e);
    if(i<0) { editor.reset(false); key_page=0; view=View::Name; dirty=true; return; }
    if(static_cast<size_t>(i)>=nets.size() || nets[i].security==wc::Security::Unsupported) return;
    ssid=nets[i].ssid;
    selected_open=nets[i].security==wc::Security::Open;
    if(selected_open) {
        if(wc::begin(ssid,"",true)) { view=View::Result; dirty=true; }
    } else { editor.reset(true); key_page=0; view=View::Password; dirty=true; }
}
void scan_event(lv_event_t *) { activity(); refused=false; wc::start_scan(); dirty=true; }
void result_event(lv_event_t *e) {
    activity();
    switch(event_id(e)) {
    case 1: wc::keep(); break;
    case 2: case 4: wc::revert(); break;   // going back: this page stays and shows how it ended
    case 3:
        editor.reset(true); key_page=0;
        if(wc::start_scan()) { view=selected_open ? View::List : View::Password; dirty=true; }
        break;
    default: wifi_close(); break;
    }
}
void back_event(lv_event_t *) {
    activity(); redact(); editor.clear();
    if(view==View::Password || view==View::Name || view==View::Security) { view=View::List; dirty=true; }
    else wifi_close();
}
void build_keyboard() {
    action(root,LV_SYMBOL_LEFT,42,10,56,56,back_event,0,true);   // x: outside the rounded corner at this height
    const std::string title=view==View::Name ? txt("输入 Wi-Fi 名称","Wi-Fi name") : ssid+txt(" 的密码"," password");
    auto *t=text_at(root,title,102,25,318,&btr_ui_20,P().wifi);
    lv_label_set_long_mode(t,LV_LABEL_LONG_DOT); lv_obj_set_height(t,26);
    // A password leaves room for the eye beside the field, level with the right edge of the keys.
    const int field_w=editor.password() ? 370 : 430;
    auto *field=panel(root,field_w,56,P().card,P().accent,16); lv_obj_set_pos(field,25,66);
    input=text_at(field,"",14,17,field_w-100,&btr_ui_20,P().ink);
    lv_label_set_long_mode(input,LV_LABEL_LONG_CLIP); lv_obj_set_height(input,27);
    count=text_at(field,"",field_w-84,20,72,&btr_ui_16,P().muted);
    lv_obj_set_style_text_align(count,LV_TEXT_ALIGN_RIGHT,0);
    if(editor.password()) {
        eye=action(root,LV_SYMBOL_EYE_OPEN,399,66,56,56,[](lv_event_t *) {},0,true);
        lv_obj_add_event_cb(eye,reveal_event,LV_EVENT_ALL,nullptr);
    }
    hint=text_at(root,"",25,130,430,&btr_ui_16,P().muted);
    lv_obj_set_style_text_align(hint,LV_TEXT_ALIGN_CENTER,0);
    auto rows=Keyboard::rows(key_page);
    for(int r=0;r<4;++r) {
        const int offset=r==3 ? 1 : 0;
        for(size_t c=0;c<rows[r].size();++c)
            action(root,std::string(1,rows[r][c]),25+(static_cast<int>(c)+offset)*62,156+r*60,58,56,key_event,rows[r][c]);
    }
    auto *shift=action(root,key_page<2 ? LV_SYMBOL_UP : key_page==2 ? "1/2" : "2/2",25,336,58,56,key_event,key_page<2 ? 256 : 258,key_page<2);
    if(key_page==1) { set_bg(shift,P().selected); set_border(shift,P().accent); }
    auto *del=action(root,LV_SYMBOL_BACKSPACE,397,336,58,56,key_event,259,true);
    lv_obj_add_event_cb(del,delete_event,LV_EVENT_ALL,nullptr);
    action(root,key_page<2 ? "123" : "abc",25,396,120,56,key_event,257);
    action(root,txt("空格","Space"),149,396,182,56,key_event,' ');
    done=action(root,txt("完成","Done"),335,396,120,56,submit_editor);
    set_bg(done,P().selected); set_border(done,P().accent);
    refresh_input();
}
void build_list() {
    action(root,LV_SYMBOL_LEFT,30,54,56,56,back_event,0,true);
    text_at(root,txt("更换 Wi-Fi","Change Wi-Fi"),90,72,245,&btr_ui_20,P().wifi);
    auto *rescan=action(root,txt("重扫","Scan"),350,54,100,56,scan_event);
    if(wc::status().phase==wc::Phase::Scanning) lv_obj_add_state(rescan,LV_STATE_DISABLED);
    auto *list=panel(root,432,330,P().card,P().edge,24); lv_obj_set_pos(list,24,114);
    lv_obj_set_style_pad_all(list,8,0); lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list,0,0); lv_obj_add_flag(list,LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_scroll_dir(list,LV_DIR_VER);
    const auto state=wc::status();
    const auto nets=state.phase==wc::Phase::Typing ? wc::networks() : std::vector<wc::Network>{};
    if(refused) label(list,txt("这个 Wi-Fi 的加密方式和你选的不一样","This Wi-Fi uses another kind of security"),400,&btr_ui_16,P().power);
    if(state.phase==wc::Phase::Scanning || nets.empty()) {
        auto *l=label(list,wc::status().phase==wc::Phase::Scanning ? txt("正在扫描","Scanning") : txt("没有找到 Wi-Fi","No Wi-Fi found"),400,&btr_ui_20,P().muted);
        lv_obj_set_height(l,56);
    }
    for(size_t i=0;i<nets.size();++i) {
        const auto &n=nets[i];
        auto *b=action(list,n.ssid,0,0,414,56,choose_network,static_cast<int>(i));
        // Leave room for the status at the right; unknown glyphs remain selectable.
        auto *l=lv_obj_get_child(b,0); lv_obj_set_align(l,LV_ALIGN_TOP_LEFT); lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_LEFT,0); lv_obj_set_width(l,226); lv_obj_set_pos(l,8,16);
        lv_label_set_long_mode(l,LV_LABEL_LONG_DOT); lv_obj_set_height(l,26);
        std::string tag=n.security==wc::Security::Unsupported ? txt("不支持","Unsupported") : n.current ? txt("当前","Current") : n.security==wc::Security::Open ? txt("开放","Open") : "";
        auto *status=text_at(b,tag,238,19,126,&btr_ui_16,n.current ? P().accent : P().muted);
        lv_obj_set_style_text_align(status,LV_TEXT_ALIGN_RIGHT,0);
        const int bars=wc::signal_bars(n.signal);   // four bars growing to the right, as on the board Dev-WifiList
        for(int k=0;k<4;++k) {
            const uint32_t ink=k<bars ? (n.current ? P().accent : P().wifi) : P().track;
            auto *bar=panel(b,5,6+k*4,ink,ink,1); lv_obj_set_pos(bar,372+k*7,36-(6+k*4));
            lv_obj_remove_flag(bar,LV_OBJ_FLAG_CLICKABLE);
        }
        if(n.security==wc::Security::Unsupported) { lv_obj_add_state(b,LV_STATE_DISABLED); lv_obj_set_style_opa(b,LV_OPA_40,0); }
    }
    auto *other=action(list,txt("其他（手动输入名称）","Other (enter name)"),0,0,414,56,choose_network,-1);
    if(state.phase==wc::Phase::Idle) lv_obj_add_state(other,LV_STATE_DISABLED);
    label(list,txt("只显示 2.4 GHz 的 Wi-Fi","Only 2.4 GHz networks are shown"),400,&btr_ui_16,P().muted);
}
const char *failure(wc::Reason r) {
    switch(r) {
    case wc::Reason::WrongPassword:return txt("密码错误","Wrong password");
    case wc::Reason::NotFound:return txt("没有找到这个 Wi-Fi","Wi-Fi not found");
    case wc::Reason::SaveFailed:return txt("没有保存，请再试一次","Not saved. Try again");
    case wc::Reason::RestoreFailed:return txt("原来的 Wi-Fi 也没连上","Previous Wi-Fi unavailable");
    case wc::Reason::UserReverted:return txt("已改回原来的 Wi-Fi","Previous Wi-Fi restored");
    case wc::Reason::AgentUnreachable:return txt("连不上电脑","Computer unreachable");
    default:return txt("连接超时","Connection timed out");
    }
}
void build_result(const wc::Status &s) {
    const bool saved=s.phase==wc::Phase::Saved, asking=s.phase==wc::Phase::Asking, reverted=s.phase==wc::Phase::Reverted;
    auto *ring=panel(root,112,112,P().bg,saved ? P().agent : asking || reverted || s.restoring ? P().power : P().wifi,56);
    lv_obj_set_pos(ring,184,70); lv_obj_set_style_border_width(ring,8,0);
    remaining=label(ring,"",94,&btr_ui_28,P().ink); center(remaining);
    result_title=text_at(root,"",40,202,400,&btr_ui_28,saved ? P().agent : asking || reverted || s.restoring ? P().power : P().ink);
    lv_obj_set_style_text_align(result_title,LV_TEXT_ALIGN_CENTER,0);
    result_note=text_at(root,"",40,250,400,&btr_ui_20,P().muted);
    lv_obj_set_style_text_align(result_note,LV_TEXT_ALIGN_CENTER,0);
    if(s.restoring) return;   // going back takes a moment, and nothing can be pressed until it has ended
    if(asking) {
        action(root,txt("改回原来的","Revert"),40,388,194,56,result_event,2);
        action(root,txt("仍然使用","Keep"),246,388,194,56,result_event,1);
    } else if(reverted) {
        action(root,selected_open ? txt("重试","Retry") : txt("重新输入","Retry"),40,388,194,56,result_event,3);
        action(root,txt("返回","Back"),246,388,194,56,result_event);
    } else if(saved) action(root,txt("完成","Done"),40,388,400,56,result_event);
    else action(root,txt("取消","Cancel"),40,388,400,56,result_event,4);
}
void refresh_result(const wc::Status &s) {
    const bool saved=s.phase==wc::Phase::Saved, asking=s.phase==wc::Phase::Asking, reverted=s.phase==wc::Phase::Reverted;
    set_text(remaining,saved ? "OK" : reverted ? "!" : std::to_string(std::max(0,s.remaining_s)));
    const char *ended=s.reason==wc::Reason::UserReverted && !s.had_previous ? txt("已取消","Cancelled") : failure(s.reason);
    set_text(result_title,saved ? txt("已连接","Connected") : s.restoring ? (s.reason==wc::Reason::UserReverted ? txt("正在改回","Going back") : failure(s.reason)) : asking ? txt("连不上电脑","Computer unreachable") : reverted ? ended : s.phase==wc::Phase::CheckingAgent ? txt("正在连接电脑","Connecting to computer") : txt("正在连接","Connecting"));
    set_text(result_note,saved ? ssid+"\n"+txt("已保存","Saved") : s.restoring ? txt("没有保存。正在连回原来的 Wi-Fi","Not saved. Going back to the previous Wi-Fi") : asking ? txt("还没有保存。\n倒计时结束后改回原来的 Wi-Fi","Not saved.\nReverts when the timer ends") : reverted ? (s.restored ? txt("没有保存。已改回原来的 Wi-Fi。","Not saved. Previous Wi-Fi restored.") : s.had_previous ? txt("没有保存。原来的 Wi-Fi 也没连上，设备会继续重试。","Not saved. Previous Wi-Fi unavailable. Retrying.") : txt("没有保存。","Not saved.")) : txt("还没有保存。连不上会自动改回。","Not saved. Reverts if connection fails."));
}
void build(lv_obj_t *screen,const wc::Status &s) {
    redact(); delete_due=0;
    input=count=hint=done=eye=result_title=result_note=remaining=nullptr;
    if(root) lv_obj_delete(root);
    (void)screen;
    root=panel(lv_layer_top(),480,480,P().bg,P().bg,0);
    lv_obj_set_style_bg_grad_color(root,hex(P().bg2),0); lv_obj_set_style_bg_grad_dir(root,LV_GRAD_DIR_VER,0);
    lv_obj_add_flag(root,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(root,0,0);
    if(view==View::Name || view==View::Password) build_keyboard();
    else if(view==View::List) build_list();
    else if(view==View::Security) {
        action(root,LV_SYMBOL_LEFT,30,54,56,56,back_event,0,true);
        text_at(root,ssid,90,72,340,&btr_ui_20,P().wifi);
        text_at(root,txt("这个 Wi-Fi 有密码吗？","Does this Wi-Fi need a password?"),40,170,400,&btr_ui_20,P().ink);
        action(root,txt("有密码","Password"),40,260,400,56,security_event,1);
        action(root,txt("没有密码","No password"),40,328,400,56,security_event,0);
    } else { build_result(s); refresh_result(s); }
    dirty=false;
}
}
void wifi_open() {
    note_activity();
    if(!wc::available() || !wc::start_scan()) return;
    editor.clear(); ssid.clear(); view=View::List; key_page=0; refused=false;
    opened=dirty=true; closing=false; last_touch=monotonic_ms();
    request_update();
}
void wifi_close() {
    redact(); editor.clear(); delete_due=0; wc::cancel(); closing=true; request_update();
}
bool wifi_opened() { return opened; }
lv_obj_t *wifi_reveal_object() { return eye; }
void wifi_reset() {
    redact(); editor.clear(); ssid.clear();
    if (root) lv_obj_delete(root);
    root=input=count=hint=done=eye=result_title=result_note=remaining=nullptr;
    opened=dirty=closing=refused=last_restoring=false; delete_due=0; last_phase=wc::Phase::Idle;
}
void wifi_tick(lv_obj_t *screen,int64_t now,bool blocked,bool pairing) {
    if(!opened) return;
    if(blocked || pairing) { redact(); delete_due=0; }
    const auto s=wc::status();
    const bool scan_failed=s.phase==wc::Phase::Idle && s.reason==wc::Reason::ScanFailed;
    if(scan_failed && view!=View::List) { redact(); editor.clear(); view=View::List; dirty=true; }
    if(closing || pairing || (s.phase==wc::Phase::Idle && !scan_failed) || (view!=View::Result && now-last_touch>=180000)) {
        redact(); editor.clear(); if(root) lv_obj_delete(root);
        root=input=count=hint=done=eye=result_title=result_note=remaining=nullptr;
        opened=dirty=closing=false; wc::cancel(); return;
    }
    if(s.phase==wc::Phase::Testing || s.phase==wc::Phase::CheckingAgent || s.phase==wc::Phase::Asking || s.phase==wc::Phase::Saved || s.phase==wc::Phase::Reverted) {
        if(view!=View::Result) { redact(); editor.clear(); view=View::Result; dirty=true; }
    }
    if(view==View::Result && s.phase==wc::Phase::Typing && s.reason==wc::Reason::InvalidInput) { refused=true; view=View::List; dirty=true; }
    if(s.phase!=last_phase || s.reason!=last_reason || s.restoring!=last_restoring) dirty=true;
    last_phase=s.phase; last_reason=s.reason; last_restoring=s.restoring;
    if(dirty) build(screen,s);
    if(delete_due && !blocked && now>=delete_due) { editor.erase(); delete_due=now+80; refresh_input(); }
    if(view==View::Result) refresh_result(s);
}
}
