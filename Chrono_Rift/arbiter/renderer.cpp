/*
 * renderer.cpp — Interactive SFML GUI for Chrono Rift
 * Tabs: Battle | Inventory | Artifacts
 * All player input via mouse clicks → shared memory → HIP/multiplayer reads
 */
#include "shared_types.h"
#include "shm_utils.h"
#include "inventory.h"
#include "game_log.h"

#include <SFML/Graphics.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <string>
#include <algorithm>
#include <vector>

static GameState* gs = nullptr;
static bool running = true;
static void handle_sigterm(int) { running = false; }

// ─── UI State ─────────────────────────────────────────────────────────────────
enum UIState { ST_IDLE, ST_ACTION, ST_TARGET, ST_WEAPON, ST_LTS, ST_LOOT };
enum Tab { TAB_BATTLE, TAB_INVENTORY, TAB_ARTIFACTS };

static UIState ui_state = ST_IDLE;
static Tab     active_tab = TAB_BATTLE;
static ActionType chosen_action = ActionType::SKIP;
static WeaponID   chosen_weapon = WeaponID::NONE;
static int     my_start = 0, my_count = 99; // which players this renderer controls
static int     last_actor = -1;
static int     menu_actor = -1; // actor who opened the current action menu (persists across sub-menus)

// ─── Colors ───────────────────────────────────────────────────────────────────
static const sf::Color C_BG(18,18,28);
static const sf::Color C_PANEL(28,28,46,230);
static const sf::Color C_TITLE(100,200,255);
static const sf::Color C_HERO(80,220,120);
static const sf::Color C_ENEMY(255,80,80);
static const sf::Color C_HP_G(60,200,80), C_HP_Y(230,200,50), C_HP_R(220,50,50);
static const sf::Color C_STA(80,160,255);
static const sf::Color C_BAR_BG(50,50,70);
static const sf::Color C_DEAD(100,100,100);
static const sf::Color C_STUN(200,180,50);
static const sf::Color C_LOG(180,180,200);
static const sf::Color C_ART(220,160,255);
static const sf::Color C_GOLD(255,200,60);
static const sf::Color C_BTN(40,50,80);
static const sf::Color C_BTN_H(60,80,140);
static const sf::Color C_BTN_D(25,25,40);
static const sf::Color C_TAB(35,35,55);
static const sf::Color C_TAB_A(50,80,160);
static const sf::Color C_WHITE(220,220,235);
static const sf::Color C_DIM(140,140,160);

// ─── Button ───────────────────────────────────────────────────────────────────
struct Btn {
    float x,y,w,h; std::string text; int id; bool enabled;
    bool contains(float mx,float my) const { return mx>=x&&mx<=x+w&&my>=y&&my<=y+h; }
};
static std::vector<Btn> buttons;

static void addBtn(float x,float y,float w,float h,const std::string& t,int id,bool en=true) {
    buttons.push_back({x,y,w,h,t,id,en});
}

// ─── Artifact / Loot helpers (mirror of arbiter logic) ────────────────────────
static bool is_artifact_wid(WeaponID wid) {
    return wid == WeaponID::SOLAR_CORE || wid == WeaponID::LUNAR_BLADE || wid == WeaponID::ECLIPSE_RELIC;
}
static void update_artifact_owner_r(WeaponID wid, int new_owner) {
    for (int i = 0; i < 3; ++i)
        if (gs->resource_table.artifacts[i].weapon_id == wid) { gs->resource_table.artifacts[i].held_by = new_owner; return; }
}
static void handleLootPickup(int loot_idx, bool take) {
    PendingLoot& loot = gs->pending_loot;
    if (loot_idx < 0 || loot_idx >= loot.count) return;
    WeaponID w = loot.weapons[loot_idx];
    Entity& killer = gs->entities[loot.for_player];
    const WeaponInfo& wi = getWeaponInfo(w);

    if (take) {
        // Duplicates are allowed — notify player if they already have one
        if (killer.inventory.hasWeapon(w)) {
            game_log(gs, "%s already has %s in inventory — picking up another copy!",
                     killer.name, wi.name);
        }
        // Try to place into primary inventory (evicts to LTS if needed)
        int pos = killer.inventory.placeWeapon(w);
        if (pos >= 0) {
            game_log(gs, "%s picks up %s!", killer.name, wi.name);
            if (is_artifact_wid(w)) {
                update_artifact_owner_r(w, loot.for_player);
                game_log(gs, "[ARTIFACT] %s now holds %s!", killer.name, wi.name);
            }
        } else if (killer.inventory.lts_count < MAX_LTS_WEAPONS) {
            // Primary full even after eviction — store directly in LTS
            killer.inventory.lts[killer.inventory.lts_count++] = w;
            game_log(gs, "%s stores %s in Long-Term Storage.", killer.name, wi.name);
            if (is_artifact_wid(w)) update_artifact_owner_r(w, loot.for_player);
        } else {
            game_log(gs, "%s's inventory is completely full! %s is lost.",
                     killer.name, wi.name);
            if (is_artifact_wid(w)) update_artifact_owner_r(w, -1);
        }
    } else {
        // Declined — a nearby alive enemy picks it up instead
        int enemy_recv = -1;
        for (int i = 0; i < gs->enemy_count; ++i) {
            int eid = MAX_PLAYERS + i;
            if (gs->entities[eid].is_alive()) { enemy_recv = eid; break; }
        }
        if (enemy_recv >= 0) {
            Entity& enemy = gs->entities[enemy_recv];
            int pos = enemy.inventory.placeWeapon(w);
            if (pos >= 0) {
                game_log(gs, "%s declines %s — %s snatches it!",
                         killer.name, wi.name, enemy.name);
                if (is_artifact_wid(w)) {
                    update_artifact_owner_r(w, enemy_recv);
                    game_log(gs, "[ARTIFACT] %s now holds %s!", enemy.name, wi.name);
                }
            } else {
                game_log(gs, "%s declines %s — no room anywhere, weapon lost!",
                         killer.name, wi.name);
                if (is_artifact_wid(w)) update_artifact_owner_r(w, -1);
            }
        } else {
            game_log(gs, "%s declines %s — no enemies alive, weapon lost!",
                     killer.name, wi.name);
            if (is_artifact_wid(w)) update_artifact_owner_r(w, -1);
        }
    }
    // Remove this entry from pending loot
    for (int i = loot_idx; i < loot.count - 1; ++i) loot.weapons[i] = loot.weapons[i + 1];
    loot.count--;
    if (loot.count <= 0) loot.active = false;
}

static void drawBtn(sf::RenderWindow& win, sf::Font& font, const Btn& b, float mx, float my) {
    bool hov = b.contains(mx,my) && b.enabled;
    sf::Color c = !b.enabled ? C_BTN_D : hov ? C_BTN_H : C_BTN;
    sf::RectangleShape r(sf::Vector2f(b.w,b.h));
    r.setPosition(b.x,b.y); r.setFillColor(c);
    r.setOutlineColor(hov?C_TITLE:sf::Color(60,60,90)); r.setOutlineThickness(1);
    win.draw(r);
    sf::Text t(b.text,font,12);
    t.setFillColor(b.enabled?C_WHITE:C_DIM);
    float tx=b.x+(b.w-t.getLocalBounds().width)/2, ty=b.y+(b.h-16)/2;
    t.setPosition(tx,ty); win.draw(t);
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
static void drawRect(sf::RenderWindow& w,float x,float y,float ww,float hh,
                     sf::Color f,sf::Color o=sf::Color::Transparent,float ot=0) {
    sf::RectangleShape r(sf::Vector2f(ww,hh));
    r.setPosition(x,y); r.setFillColor(f);
    if(ot>0){r.setOutlineColor(o);r.setOutlineThickness(ot);}
    w.draw(r);
}
static void drawBar(sf::RenderWindow& w,float x,float y,float bw,float bh,
                    int v,int mx,sf::Color c) {
    drawRect(w,x,y,bw,bh,C_BAR_BG);
    if(mx>0&&v>0) drawRect(w,x,y,bw*std::min(1.f,(float)v/mx),bh,c);
}
static sf::Color hpC(int hp,int mx) {
    if(mx<=0)return C_HP_R; int p=hp*100/mx;
    return p<25?C_HP_R:p<50?C_HP_Y:C_HP_G;
}

// ─── Submit GUI action ───────────────────────────────────────────────────────
static void submitAction(int eid, ActionType type, int target=-1, WeaponID wid=WeaponID::NONE) {
    if(eid<0||eid>=gs->player_count) return;
    pthread_mutex_lock(&gs->gui_lock[eid]);
    gs->gui_input[eid].action.actor_id = eid;
    gs->gui_input[eid].action.type = type;
    gs->gui_input[eid].action.target_id = target;
    gs->gui_input[eid].action.weapon_id = wid;
    gs->gui_input[eid].action.valid = true;
    gs->gui_input[eid].ready = true;
    pthread_cond_signal(&gs->gui_cond[eid]);
    pthread_mutex_unlock(&gs->gui_lock[eid]);
    ui_state = ST_IDLE;
    last_actor = eid;
}

// ─── Draw Entity Card ────────────────────────────────────────────────────────
static void drawEntity(sf::RenderWindow& win,sf::Font& font,const Entity& e,
                       bool cur,float x,float y,float cw,bool hero) {
    if(cur) drawRect(win,x-3,y-2,cw+6,66,sf::Color(0,200,255,30));
    sf::Text nm(e.name,font,13); nm.setStyle(sf::Text::Bold);
    nm.setFillColor(e.state==EntityState::DEAD?C_DEAD:e.stun_active?C_STUN:hero?C_HERO:C_ENEMY);
    nm.setPosition(x,y); win.draw(nm);
    if(e.state==EntityState::DEAD){
        sf::Text d("DEFEATED",font,10);d.setFillColor(C_HP_R);d.setPosition(x+cw-65,y+2);win.draw(d);return;
    }
    char ds[32];snprintf(ds,32,"DMG:%d",e.damage);
    sf::Text dt(ds,font,9);dt.setFillColor(C_DIM);dt.setPosition(x+cw-55,y+2);win.draw(dt);
    if(e.stun_active){sf::Text st("[STUN]",font,9);st.setFillColor(C_STUN);st.setPosition(x+cw-110,y+2);win.draw(st);}
    float by=y+18;
    sf::Text hl("HP",font,9);hl.setFillColor(C_DIM);hl.setPosition(x,by);win.draw(hl);
    drawBar(win,x+20,by+1,cw-90,11,e.hp,e.max_hp,hpC(e.hp,e.max_hp));
    char hs[32];snprintf(hs,32,"%d/%d",e.hp,e.max_hp);
    sf::Text hv(hs,font,9);hv.setFillColor(C_WHITE);hv.setPosition(x+cw-65,by);win.draw(hv);
    float sy=by+15;
    sf::Text sl("STA",font,9);sl.setFillColor(C_DIM);sl.setPosition(x,sy);win.draw(sl);
    drawBar(win,x+20,sy+1,cw-90,11,e.stamina,e.max_stamina,C_STA);
    char ss[32];snprintf(ss,32,"%d/%d",e.stamina,e.max_stamina);
    sf::Text sv(ss,font,9);sv.setFillColor(C_WHITE);sv.setPosition(x+cw-65,sy);win.draw(sv);
    // weapons
    std::string ws;
    for(int i=0;i<INVENTORY_SLOTS;++i){
        WeaponID w=e.inventory.slots[i]; if(w==WeaponID::NONE)continue;
        if(i>0&&e.inventory.slots[i-1]==w)continue;
        if(!ws.empty())ws+=", "; ws+=getWeaponInfo(w).name;
    }
    if(!ws.empty()){
        sf::Text wt(ws,font,8);
        bool art=e.inventory.hasWeapon(WeaponID::SOLAR_CORE)||e.inventory.hasWeapon(WeaponID::LUNAR_BLADE)||e.inventory.hasWeapon(WeaponID::ECLIPSE_RELIC);
        wt.setFillColor(art?C_ART:C_DIM);wt.setPosition(x,sy+14);win.draw(wt);
    }
}

// ─── Draw Tab Bar ─────────────────────────────────────────────────────────────
static void drawTabs(sf::RenderWindow& win,sf::Font& font,float x,float y,float w,float mx,float my){
    const char* names[]={"BATTLE","INVENTORY","ARTIFACTS"};
    float tw=w/3;
    for(int i=0;i<3;++i){
        float tx=x+i*tw;
        bool act=(active_tab==(Tab)i);
        bool hov=mx>=tx&&mx<=tx+tw&&my>=y&&my<=y+28;
        drawRect(win,tx,y,tw-2,28,act?C_TAB_A:hov?sf::Color(45,45,70):C_TAB,
                 act?C_TITLE:sf::Color(50,50,70),1);
        sf::Text t(names[i],font,11);t.setStyle(sf::Text::Bold);
        t.setFillColor(act?C_WHITE:C_DIM);
        t.setPosition(tx+(tw-t.getLocalBounds().width)/2,y+6);win.draw(t);
        addBtn(tx,y,tw-2,28,"",100+i,true);
    }
}

// ─── Draw Battle Tab ──────────────────────────────────────────────────────────
static float drawBattle(sf::RenderWindow& win,sf::Font& font,float M,float yTop,float W,float mx,float my){
    float colW=(W-3*M)/2, CARD_H=68, P=8;
    // Heroes
    float hH=26+gs->player_count*CARD_H+P;
    drawRect(win,M,yTop,colW,hH,C_PANEL,sf::Color(60,180,80,60),1);
    {sf::Text h("HEROES",font,12);h.setStyle(sf::Text::Bold);h.setFillColor(C_HERO);
     h.setPosition(M+P,yTop+5);win.draw(h);}
    float ey=yTop+24;
    for(int i=0;i<gs->player_count;++i){
        drawEntity(win,font,gs->entities[i],gs->current_actor_id==i,M+P,ey,colW-2*P,true);
        ey+=CARD_H;
    }
    // Enemies
    float eH=26+gs->enemy_count*CARD_H+P;
    float eX=M+colW+M;
    drawRect(win,eX,yTop,colW,eH,C_PANEL,sf::Color(180,60,60,60),1);
    {sf::Text h("ENEMIES",font,12);h.setStyle(sf::Text::Bold);h.setFillColor(C_ENEMY);
     h.setPosition(eX+P,yTop+5);win.draw(h);}
    ey=yTop+24;
    for(int i=0;i<gs->enemy_count;++i){
        int eid=MAX_PLAYERS+i;
        drawEntity(win,font,gs->entities[eid],gs->current_actor_id==eid,eX+P,ey,colW-2*P,false);
        // In TARGET_SELECT, make enemies clickable
        if(ui_state==ST_TARGET && gs->entities[eid].is_alive()){
            addBtn(eX+P,ey,colW-2*P,CARD_H-4,"",200+i,true);
            bool hov2=mx>=eX+P&&mx<=eX+P+colW-2*P&&my>=ey&&my<=ey+CARD_H-4;
            if(hov2) drawRect(win,eX+P-2,ey-1,colW-2*P+4,CARD_H-2,sf::Color(255,100,100,40));
        }
        ey+=CARD_H;
    }
    return yTop+std::max(hH,eH)+6;
}

// ─── Draw Action Menu ─────────────────────────────────────────────────────────
static float drawActions(sf::RenderWindow& win,sf::Font& font,float M,float y,float W,float mx,float my,int actor){
    Entity& e=gs->entities[actor];
    float P=8, bw=110, bh=32, gap=6;
    float panW=W-2*M, panH=82;

    char title[96];snprintf(title,96,"%s's Turn  |  STA: %d/%d",e.name,e.stamina,e.max_stamina);

    if(ui_state==ST_ACTION){
        float bx=M+P,by=y+26;
        bool canAct=e.stamina>=10; // STA_MIN_ACT
        panH=104;
        // Draw panel background with correct height
        drawRect(win,M,y,panW,panH,C_PANEL,sf::Color(100,80,200,60),1);
        sf::Text tt(title,font,12);tt.setStyle(sf::Text::Bold);tt.setFillColor(C_GOLD);
        tt.setPosition(M+P,y+5);win.draw(tt);
        addBtn(bx,by,bw,bh,"Strike -25",1,canAct&&e.stamina>=25); bx+=bw+gap;
        addBtn(bx,by,bw,bh,"Exhaust -20",2,canAct&&e.stamina>=20); bx+=bw+gap;
        addBtn(bx,by,bw,bh,"Weapon -30",3,canAct&&e.stamina>=30); bx+=bw+gap;
        addBtn(bx,by,bw,bh,"Heal -15",5,canAct&&e.stamina>=15); bx+=bw+gap;
        addBtn(bx,by,bw,bh,"Skip +40",6); bx+=bw+gap;
        bool canUlt=e.inventory.hasWeapon(WeaponID::SOLAR_CORE)&&e.inventory.hasWeapon(WeaponID::LUNAR_BLADE)&&e.stamina>=50;
        addBtn(M+P,by+bh+4,bw,bh,"ULTIMATE -50",7,canUlt);
        addBtn(M+P+bw+gap,by+bh+4,bw,bh,"Swap In -10",4,e.inventory.lts_count>0&&e.stamina>=10);
        addBtn(M+P+2*(bw+gap),by+bh+4,bw,bh,"Quit",9);
    } else if(ui_state==ST_TARGET){
        panH=78;
        drawRect(win,M,y,panW,panH,C_PANEL,sf::Color(100,80,200,60),1);
        sf::Text tt(title,font,12);tt.setStyle(sf::Text::Bold);tt.setFillColor(C_GOLD);
        tt.setPosition(M+P,y+5);win.draw(tt);
        sf::Text t("Click an enemy to target",font,11);t.setFillColor(C_GOLD);
        t.setPosition(M+P,y+26);win.draw(t);
        addBtn(M+P,y+46,80,26,"Cancel",50);
    } else if(ui_state==ST_WEAPON){
        drawRect(win,M,y,panW,panH,C_PANEL,sf::Color(100,80,200,60),1);
        sf::Text tt(title,font,12);tt.setStyle(sf::Text::Bold);tt.setFillColor(C_GOLD);
        tt.setPosition(M+P,y+5);win.draw(tt);
        sf::Text t("Select weapon from inventory:",font,11);t.setFillColor(C_GOLD);
        t.setPosition(M+P,y+26);win.draw(t);
        float bx=M+P,by=y+44; int cnt=0;
        for(int i=0;i<INVENTORY_SLOTS;++i){
            WeaponID w=e.inventory.slots[i]; if(w==WeaponID::NONE)continue;
            if(i>0&&e.inventory.slots[i-1]==w)continue;
            char lbl[48];snprintf(lbl,48,"%s (%d)",getWeaponInfo(w).name,getWeaponInfo(w).damage);
            addBtn(bx,by,140,26,lbl,300+(int)w); bx+=146;
            if(++cnt%4==0){bx=M+P;by+=30;}
        }
        addBtn(bx,by,80,26,"Cancel",50); panH=std::max(78.f,by+30-y);
    } else if(ui_state==ST_LTS){
        drawRect(win,M,y,panW,panH,C_PANEL,sf::Color(100,80,200,60),1);
        sf::Text tt(title,font,12);tt.setStyle(sf::Text::Bold);tt.setFillColor(C_GOLD);
        tt.setPosition(M+P,y+5);win.draw(tt);
        sf::Text t("Select weapon from Long-Term Storage:",font,11);t.setFillColor(C_GOLD);
        t.setPosition(M+P,y+26);win.draw(t);
        float bx=M+P,by=y+44;
        for(int i=0;i<e.inventory.lts_count;++i){
            const WeaponInfo& wi=getWeaponInfo(e.inventory.lts[i]);
            addBtn(bx,by,140,26,wi.name,400+(int)e.inventory.lts[i]); bx+=146;
            if((i+1)%4==0){bx=M+P;by+=30;}
        }
        addBtn(bx,by,80,26,"Cancel",50); panH=std::max(78.f,by+30-y);
    } else {
        // ST_IDLE or other - just draw a minimal panel
        drawRect(win,M,y,panW,panH,C_PANEL,sf::Color(100,80,200,60),1);
        sf::Text tt(title,font,12);tt.setStyle(sf::Text::Bold);tt.setFillColor(C_GOLD);
        tt.setPosition(M+P,y+5);win.draw(tt);
    }
    return y+panH+6;
}

// ─── Draw Inventory Tab ───────────────────────────────────────────────────────
static float drawInventory(sf::RenderWindow& win,sf::Font& font,float M,float y,float W,float mx,float my){
    float P=8, panW=W-2*M;
    int heroCount=gs->player_count;
    if(heroCount<1) heroCount=1;
    float colW=(panW-(heroCount-1)*P)/heroCount;

    float maxBottom=y;
    for(int pi=0;pi<heroCount;++pi){
        Entity& e=gs->entities[pi];
        float cx=M+pi*(colW+P);
        float cy=y;

        // Hero header
        sf::Color borderC=pi==0?sf::Color(60,180,80,80):sf::Color(80,120,220,80);
        drawRect(win,cx,cy,colW,28,C_PANEL,borderC,1);
        sf::Text nh(e.name,font,12);nh.setStyle(sf::Text::Bold);
        nh.setFillColor(pi==0?C_HERO:sf::Color(100,180,255));
        nh.setPosition(cx+P,cy+5);win.draw(nh);
        cy+=32;

        // Primary inventory
        int used=0; for(int i=0;i<INVENTORY_SLOTS;++i) if(e.inventory.slots[i]!=WeaponID::NONE) used++;
        char hdr[64];snprintf(hdr,64,"Primary (%d/%d slots)",used,INVENTORY_SLOTS);
        float invH=80;
        drawRect(win,cx,cy,colW,invH,C_PANEL,sf::Color(60,120,80,60),1);
        sf::Text ht(hdr,font,11);ht.setStyle(sf::Text::Bold);ht.setFillColor(C_HERO);
        ht.setPosition(cx+P,cy+4);win.draw(ht);
        // Slot grid (10 cols x 2 rows for compact layout)
        int cols=10; float sw=(colW-2*P-9*(cols-1)/cols)/cols; if(sw<14)sw=14; float sh=sw;
        float sx=cx+P,sy=cy+22;
        for(int i=0;i<INVENTORY_SLOTS;++i){
            float gx=sx+(i%cols)*(sw+4), gy=sy+(i/cols)*(sh+4);
            WeaponID w=e.inventory.slots[i];
            sf::Color fc=w==WeaponID::NONE?sf::Color(30,30,45):
                         is_artifact_wid(w)?sf::Color(80,50,120):sf::Color(40,60,80);
            drawRect(win,gx,gy,sw,sh,fc,sf::Color(60,60,80),1);
            if(w!=WeaponID::NONE){
                char abbr[4]={0}; const char* n=getWeaponInfo(w).name;
                abbr[0]=n[0]; if(n[1]) abbr[1]=n[1];
                sf::Text at(abbr,font,9);at.setFillColor(C_WHITE);at.setPosition(gx+2,gy+2);win.draw(at);
                char dmgs[8];snprintf(dmgs,8,"%d",getWeaponInfo(w).damage);
                sf::Text dt(dmgs,font,7);dt.setFillColor(C_GOLD);dt.setPosition(gx+2,gy+sh-12);win.draw(dt);
            }
        }
        cy+=invH+4;

        // LTS
        drawRect(win,cx,cy,colW,70,C_PANEL,sf::Color(80,80,120,60),1);
        char lhdr[48];snprintf(lhdr,48,"LTS (%d items)",e.inventory.lts_count);
        sf::Text lt(lhdr,font,11);lt.setStyle(sf::Text::Bold);lt.setFillColor(C_STA);
        lt.setPosition(cx+P,cy+4);win.draw(lt);
        float lx=cx+P,ly=cy+22;
        for(int i=0;i<e.inventory.lts_count&&i<8;++i){
            const WeaponInfo& wi=getWeaponInfo(e.inventory.lts[i]);
            char lb[48];snprintf(lb,48,"%s (D:%d)",wi.name,wi.damage);
            sf::Text wt(lb,font,9);wt.setFillColor(C_DIM);wt.setPosition(lx,ly);win.draw(wt);
            ly+=14;
        }
        cy+=74;
        if(cy>maxBottom) maxBottom=cy;
    }
    return maxBottom;
}

// ─── Draw Artifacts Tab ───────────────────────────────────────────────────────
static float drawArtifacts(sf::RenderWindow& win,sf::Font& font,float M,float y,float W,float mx,float my){
    float P=8,panW=W-2*M,cardW=(panW-2*P-20)/3;
    drawRect(win,M,y,panW,200,C_PANEL,sf::Color(160,100,220,60),1);
    sf::Text hdr("ARTIFACT RESOURCES",font,13);hdr.setStyle(sf::Text::Bold);
    hdr.setFillColor(C_ART);hdr.setPosition(M+P,y+5);win.draw(hdr);
    const char* names[]={"Solar Core","Lunar Blade","Eclipse Relic"};
    const char* icons[]={"S","L","E"};
    for(int a=0;a<3;++a){
        float cx=M+P+a*(cardW+10), cy=y+28;
        const ArtifactEntry& art=gs->resource_table.artifacts[a];
        const WeaponInfo& wi=getWeaponInfo(art.weapon_id);
        sf::Color border=!art.exists?C_DEAD:art.held_by<0?C_GOLD:C_ART;
        drawRect(win,cx,cy,cardW,155,sf::Color(25,25,40),border,2);
        // Icon
        sf::Text ic(icons[a],font,28);ic.setStyle(sf::Text::Bold);ic.setFillColor(border);
        ic.setPosition(cx+cardW/2-10,cy+5);win.draw(ic);
        // Name
        sf::Text nm(names[a],font,13);nm.setStyle(sf::Text::Bold);nm.setFillColor(C_WHITE);
        nm.setPosition(cx+(cardW-nm.getLocalBounds().width)/2,cy+42);win.draw(nm);
        char info[48];snprintf(info,48,"DMG: %d  |  Slots: %d",wi.damage,wi.slot_size);
        sf::Text it(info,font,10);it.setFillColor(C_DIM);
        it.setPosition(cx+(cardW-it.getLocalBounds().width)/2,cy+62);win.draw(it);
        // Status
        const char* status; sf::Color sc;
        if(!art.exists){status="Not Yet Spawned";sc=C_DEAD;}
        else if(art.held_by<0){status="UNCLAIMED";sc=C_GOLD;}
        else{status=gs->entities[art.held_by].name;sc=C_ART;}
        sf::Text st(status,font,12);st.setStyle(sf::Text::Bold);st.setFillColor(sc);
        st.setPosition(cx+(cardW-st.getLocalBounds().width)/2,cy+90);win.draw(st);
        if(art.exists&&art.held_by>=0){
            sf::Text hl("Held by:",font,9);hl.setFillColor(C_DIM);
            hl.setPosition(cx+(cardW-hl.getLocalBounds().width)/2,cy+78);win.draw(hl);
        }
        // Deadlock info
        if(art.exists&&art.held_by>=0){
            int h=art.held_by;
            int waiting=gs->waiting_for_artifact[h];
            if(waiting>=0){
                char wl[64];snprintf(wl,64,"Wants: %s",getWeaponInfo((WeaponID)waiting).name);
                sf::Text wt(wl,font,9);wt.setFillColor(C_HP_R);
                wt.setPosition(cx+(cardW-wt.getLocalBounds().width)/2,cy+115);win.draw(wt);
            }
        }
    }
    return y+206;
}

// ─── Draw Action Log ──────────────────────────────────────────────────────────
static void drawLog(sf::RenderWindow& win,sf::Font& font,float M,float y,float W,float H){
    float logH=H-y-M;
    if(logH<40) logH=40;
    drawRect(win,M,y,W-2*M,logH,C_PANEL,sf::Color(80,80,120,60),1);
    sf::Text hdr("ACTION LOG",font,11);hdr.setStyle(sf::Text::Bold);hdr.setFillColor(C_TITLE);
    hdr.setPosition(M+8,y+4);win.draw(hdr);
    pthread_mutex_lock(&gs->log_lock);
    int maxL=(int)((logH-22)/14); if(maxL<1)maxL=1;
    int show=gs->log_count<maxL?gs->log_count:maxL;
    int start=gs->log_count-show;
    float ly=y+20;
    for(int i=0;i<show;++i){
        int idx=start+i;
        if(idx>=0&&idx<ACTION_LOG_LINES){
            sf::Text lt(gs->log_lines[idx],font,10);lt.setFillColor(C_LOG);
            lt.setPosition(M+8,ly);win.draw(lt);ly+=14;
        }
    }
    pthread_mutex_unlock(&gs->log_lock);
}

// ─── Draw Loot Overlay ────────────────────────────────────────────────────────
static float drawLoot(sf::RenderWindow& win,sf::Font& font,float M,float y,float W,float mx,float my){
    PendingLoot& loot=gs->pending_loot;
    if(!loot.active||loot.count<=0) return y;
    float P=8,panW=W-2*M,panH=50+loot.count*38;
    drawRect(win,M,y,panW,panH,sf::Color(40,25,60,245),sf::Color(220,160,255,200),2);
    char title[96];snprintf(title,96,"LOOT DROPPED — %s, choose:",gs->entities[loot.for_player].name);
    sf::Text tt(title,font,13);tt.setStyle(sf::Text::Bold);tt.setFillColor(C_GOLD);
    tt.setPosition(M+P,y+6);win.draw(tt);
    float by=y+32;
    Entity& loot_player = gs->entities[loot.for_player];
    for(int i=0;i<loot.count;++i){
        const WeaponInfo& wi=getWeaponInfo(loot.weapons[i]);
        bool alreadyOwned = loot_player.inventory.hasWeapon(loot.weapons[i]);
        char lbl[96];
        if(alreadyOwned)
            snprintf(lbl,96,"%s  (DMG:%d  Slots:%d)  [ALREADY IN INVENTORY]",wi.name,wi.damage,wi.slot_size);
        else
            snprintf(lbl,96,"%s  (DMG:%d  Slots:%d)",wi.name,wi.damage,wi.slot_size);
        sf::Text wt(lbl,font,11);
        sf::Color txtColor = alreadyOwned ? sf::Color(255,200,100) :
                             (is_artifact_wid(loot.weapons[i]) ? C_ART : C_WHITE);
        wt.setFillColor(txtColor);
        wt.setPosition(M+P,by+5);win.draw(wt);
        // Pick Up always enabled — duplicates are allowed
        addBtn(M+panW-190,by,88,28,"Pick Up",700+i,true);
        addBtn(M+panW-96,by,88,28,"Decline",750+i,true);
        by+=38;
    }
    return y+panH+6;
}

// ─── Handle Click ─────────────────────────────────────────────────────────────
static void handleClick(float mx,float my){
    // Use menu_actor for sub-menu states so we don't lose track when
    // current_actor_id changes between frames (race with arbiter)
    int actor = (ui_state != ST_IDLE && ui_state != ST_ACTION && menu_actor >= 0)
                ? menu_actor : gs->current_actor_id;
    for(auto& b:buttons){
        if(!b.enabled||!b.contains(mx,my)) continue;
        int id=b.id;
        // Tab clicks
        if(id>=100&&id<103){active_tab=(Tab)(id-100);return;}
        // Player selector (inventory tab)
        if(id>=600&&id<600+MAX_PLAYERS){gs->gui_viewing_player=id-600;return;}
        // Cancel
        if(id==50){ui_state=ST_ACTION;return;}
        // Loot pickup
        if(id>=700&&id<750){handleLootPickup(id-700,true);return;}
        // Loot decline
        if(id>=750&&id<800){handleLootPickup(id-750,false);return;}
        // Action buttons
        if(ui_state==ST_ACTION&&actor>=0&&actor<gs->player_count){
            switch(id){
                case 1: chosen_action=ActionType::STRIKE;menu_actor=actor;ui_state=ST_TARGET;return;
                case 2: chosen_action=ActionType::EXHAUST;menu_actor=actor;ui_state=ST_TARGET;return;
                case 3: menu_actor=actor;ui_state=ST_WEAPON;return;
                case 4: menu_actor=actor;ui_state=ST_LTS;return;
                case 5: submitAction(actor,ActionType::HEAL);menu_actor=-1;return;
                case 6: submitAction(actor,ActionType::SKIP);menu_actor=-1;return;
                case 7: submitAction(actor,ActionType::ULTIMATE);menu_actor=-1;return;

                case 9: submitAction(actor,ActionType::QUIT);menu_actor=-1;return;
            }
        }
        // Target selection (enemy clicked)
        if(ui_state==ST_TARGET&&id>=200&&id<200+MAX_ENEMIES){
            int eid=MAX_PLAYERS+(id-200);
            if(chosen_action==ActionType::USE_WEAPON)
                submitAction(actor,ActionType::USE_WEAPON,eid,chosen_weapon);
            else
                submitAction(actor,chosen_action,eid);
            menu_actor=-1;
            return;
        }
        // Weapon selection
        if(ui_state==ST_WEAPON&&id>=300&&id<400){
            chosen_weapon=(WeaponID)(id-300);
            chosen_action=ActionType::USE_WEAPON;
            ui_state=ST_TARGET;return;
        }
        // LTS selection
        if(ui_state==ST_LTS&&id>=400&&id<500){
            submitAction(actor,ActionType::SWAP_IN,-1,(WeaponID)(id-400));menu_actor=-1;return;
        }
        // Artifact selection

    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int argc,char* argv[]){
    gs=shm_attach(false);
    if(!gs){fprintf(stderr,"[RENDERER] Failed to attach shm\n");return 1;}

    signal(SIGTERM,handle_sigterm);signal(SIGPIPE,SIG_IGN);

    // Parse optional player range
    if(argc>=4){my_start=atoi(argv[2]);my_count=atoi(argv[3]);}

    // Enable GUI mode
    gs->gui_mode=true;

    const float W=980,H=780;
    sf::RenderWindow window(sf::VideoMode((unsigned)W,(unsigned)H),"Chrono Rift",sf::Style::Close);
    window.setFramerateLimit(30);

    sf::Font font;
    const char* fps[]={"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",nullptr};
    bool loaded=false;
    for(int i=0;fps[i];++i) if(font.loadFromFile(fps[i])){loaded=true;break;}
    if(!loaded){fprintf(stderr,"[RENDERER] No font\n");shm_detach(gs);return 1;}

    float M=12;

    while(window.isOpen()&&running){
        sf::Event ev;
        sf::Vector2i mp=sf::Mouse::getPosition(window);
        float mx=(float)mp.x,my=(float)mp.y;

        while(window.pollEvent(ev)){
            if(ev.type==sf::Event::Closed) window.close();
            if(ev.type==sf::Event::MouseButtonPressed&&ev.mouseButton.button==sf::Mouse::Left)
                handleClick((float)ev.mouseButton.x,(float)ev.mouseButton.y);
        }

        // Update UI state based on whose turn it is
        int actor=gs->current_actor_id;
        bool isMyPlayer=actor>=0&&actor<gs->player_count&&actor>=my_start&&actor<my_start+my_count;
        if(isMyPlayer&&ui_state==ST_IDLE&&actor!=last_actor&&!gs->gui_input[actor].ready){
            ui_state=ST_ACTION;
            active_tab=TAB_BATTLE;
            gs->gui_viewing_player=actor;
            menu_actor=actor;
        }
        // Only reset to IDLE if we're not in a sub-menu flow (weapon/target/lts/artifact)
        // The menu_actor check prevents race conditions where current_actor_id
        // changes between frames while the player is still selecting a weapon/target
        bool inSubMenu=(ui_state==ST_WEAPON||ui_state==ST_TARGET||ui_state==ST_LTS)&&menu_actor>=0;
        if(!isMyPlayer&&ui_state!=ST_IDLE&&!inSubMenu) { ui_state=ST_IDLE; menu_actor=-1; }
        // If in sub-menu, use menu_actor for rendering instead of current_actor_id
        if(inSubMenu) actor=menu_actor;
        if(actor!=last_actor) last_actor=-1; // allow new action menu

        buttons.clear();
        window.clear(C_BG);

        // Title bar
        drawRect(window,M,M,W-2*M,40,C_PANEL,sf::Color(60,120,180,80),1);
        {sf::Text t("CHRONO RIFT",font,18);t.setStyle(sf::Text::Bold);t.setFillColor(C_TITLE);
         t.setPosition(M+8,M+9);window.draw(t);
         char ks[48];snprintf(ks,48,"Enemies Slain: %d / %d",gs->total_enemies_killed,WIN_KILL_COUNT);
         sf::Text k(ks,font,12);k.setFillColor(sf::Color(180,220,255));
         k.setPosition(W-M-8-k.getLocalBounds().width,M+12);window.draw(k);}

        if(gs->ultimate_pause_active){
            drawRect(window,M,M+42,W-2*M,20,sf::Color(40,120,40,180));
            sf::Text u("*** ULTIMATE ACTIVE - ENEMIES FROZEN ***",font,11);
            u.setStyle(sf::Text::Bold);u.setFillColor(sf::Color(120,255,120));
            u.setPosition(W/2-u.getLocalBounds().width/2,M+44);window.draw(u);
        }

        float yy=M+42+(gs->ultimate_pause_active?22:0)+4;

        // Tab bar
        drawTabs(window,font,M,yy,W-2*M,mx,my);
        yy+=34;

        // Tab content
        float contentEnd=yy;
        if(active_tab==TAB_BATTLE){
            contentEnd=drawBattle(window,font,M,yy,W,mx,my);
            // Loot overlay (shown above action menu when loot is pending)
            if(gs->pending_loot.active&&gs->pending_loot.count>0)
                contentEnd=drawLoot(window,font,M,contentEnd,W,mx,my);
            // Action menu if it's a player's turn we control
            if((isMyPlayer||inSubMenu)&&ui_state!=ST_IDLE)
                contentEnd=drawActions(window,font,M,contentEnd,W,mx,my,inSubMenu?menu_actor:actor);
        } else if(active_tab==TAB_INVENTORY){
            contentEnd=drawInventory(window,font,M,yy,W,mx,my);
        } else {
            contentEnd=drawArtifacts(window,font,M,yy,W,mx,my);
        }

        // Action log always at bottom
        drawLog(window,font,M,contentEnd,W,H);

        // Draw all buttons
        for(auto& b:buttons) if(!b.text.empty()) drawBtn(window,font,b,mx,my);

        // Game over overlay
        if(gs->game_over){
            drawRect(window,0,0,W,H,sf::Color(0,0,0,180));
            const char* msg=gs->players_won?"VICTORY!":gs->quit_requested?"QUIT":"DEFEAT";
            sf::Color mc=gs->players_won?C_HERO:gs->quit_requested?C_GOLD:C_ENEMY;
            sf::Text mt(msg,font,32);mt.setStyle(sf::Text::Bold);mt.setFillColor(mc);
            mt.setPosition((W-mt.getLocalBounds().width)/2,H/2-20);window.draw(mt);
        }

        window.display();
        if(gs->game_over) break;
    }

    // Hold game-over screen
    if(window.isOpen()&&gs->game_over){
        sf::Clock clk;
        while(window.isOpen()&&clk.getElapsedTime().asSeconds()<5){
            sf::Event ev;while(window.pollEvent(ev)) if(ev.type==sf::Event::Closed) window.close();
            struct timespec ts={0,100000000};nanosleep(&ts,nullptr);
        }
    }
    shm_detach(gs);return 0;
}

