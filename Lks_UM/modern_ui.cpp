#include "modern_ui.hpp"
#include "overlay.hpp"
#include "hud_window.hpp"
#include "ui_render.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <windowsx.h>

extern EspSettings g_EspSettings;
extern AimSettings g_AimSettings;
extern MiscSettings g_MiscSettings;
extern HWND g_hStatus;
extern bool g_ListeningForKey;
extern std::vector<std::wstring> ModernConfigNames();
extern std::wstring ModernSelectedConfig();
extern void ModernSelectConfig(const std::wstring& name);

namespace lks::modern_ui {
using namespace dvm::ui;
using namespace dvm::theme;
namespace {
enum class Page { Aim, Esp, Misc, Config };
enum Kind { Nav, Toggle, Action, Color, Stepper, Caption };
struct Zone { int id; Kind kind; D2D1_RECT_F rect; };
struct State {
  HWND hwnd{}; Renderer r; unsigned dpi{96}; Page page{Page::Aim};
  std::vector<Zone> zones; std::vector<std::wstring> configs; int hot{-1}; int down{-1}; float scroll{}; bool french{true}; int openList{-1};
} s;

const wchar_t* tr(const wchar_t* fr,const wchar_t* en){return s.french?fr:en;}

D2D1_RECT_F box(float x,float y,float w,float h){return D2D1::RectF(x,y,x+w,y+h);}
bool inside(const D2D1_RECT_F&r,float x,float y){return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}
float mixf(float a,float b,float t){return a+(b-a)*t;}
D2D1_COLOR_F from_color(COLORREF c){return D2D1::ColorF(GetRValue(c)/255.f,GetGValue(c)/255.f,GetBValue(c)/255.f);}
void zone(int id,Kind k,D2D1_RECT_F r){s.zones.push_back({id,k,r});}
int hit(float x,float y){for(auto it=s.zones.rbegin();it!=s.zones.rend();++it)if(inside(it->rect,x,y))return it->id;return -1;}

void card(D2D1_RECT_F r){s.r.shadow(r,r_lg,8,0.28f);s.r.fill_round(r,r_lg,surface);s.r.stroke_round(r,r_lg,stroke);}
void title(const wchar_t* kicker,const wchar_t* heading,const wchar_t* sub){
  s.r.text(kicker,box(rail_w+28,58,500,18),FontMicro,accent);
  s.r.text(heading,box(rail_w+28,78,700,38),FontDisplay,text_hi);
  s.r.text(sub,box(rail_w+28,118,760,24),FontBody,text_dim);
}
void section(const wchar_t* t,float x,float y,float w){s.r.text(t,box(x,y,w,22),FontSubtitle,text_hi);}

void toggle(int id,const wchar_t* name,const wchar_t* desc,bool on,float x,float y,float w){
  auto r=box(x,y,w,58); zone(id,Toggle,r);
  if(s.hot==id)s.r.fill_round(r,r_md,surface_hover);
  s.r.text(name,box(x+14,y+9,w-90,20),FontBodyStrong,text_hi);
  s.r.text(desc,box(x+14,y+31,w-90,17),FontCaption,text_dim);
  auto tr=box(x+w-62,y+17,44,24);s.r.fill_round(tr,12,on?accent:stroke_strong);
  s.r.fill_circle(on?tr.right-12:tr.left+12,tr.top+12,8,on?accent_ink:text);
}
void action(int id,const wchar_t* textValue,float x,float y,float w,bool primary=false,bool dangerButton=false){
  auto r=box(x,y,w,42);zone(id,Action,r);
  auto c=primary?accent:(s.hot==id?surface_hover:surface_active);
  s.r.fill_round(r,r_md,c);if(!primary)s.r.stroke_round(r,r_md,dangerButton&&s.hot==id?danger:stroke_strong);
  s.r.text(textValue,r,FontBodyStrong,primary?accent_ink:(dangerButton&&s.hot==id?danger:text_hi),AlignCenter);
}
void color_row(int id,const wchar_t* name,COLORREF value,float x,float y,float w){
  auto r=box(x,y,w,48);zone(id,Color,r);if(s.hot==id)s.r.fill_round(r,r_md,surface_hover);
  s.r.text(name,box(x+14,y,w-70,48),FontBody,text);
  auto sw=box(x+w-50,y+10,30,28);s.r.fill_round(sw,8,from_color(value));s.r.stroke_round(sw,8,s.hot==id?accent:stroke_strong);
}
void slider(int id,const wchar_t* name,float value,float minv,float maxv,const wchar_t* suffix,float x,float y,float w){
  auto r=box(x,y,w,56);zone(id,Stepper,r);
  s.r.text(name,box(x+14,y+4,w-110,22),FontBody,text);
  wchar_t b[48]{};swprintf_s(b,L"%.1f%s",value,suffix);s.r.text(b,box(x+w-94,y+4,76,22),FontBodyStrong,text_hi,AlignRight);
  auto track=box(x+14,y+37,w-32,5);s.r.fill_round(track,3,stroke_strong);float t=std::clamp((value-minv)/(maxv-minv),0.f,1.f);
  s.r.fill_round(box(track.left,track.top,(track.right-track.left)*t,5),3,accent);s.r.fill_circle(track.left+(track.right-track.left)*t,track.top+2.5f,8,s.hot==id||s.down==id?accent_hi:accent);
}
void seg(int firstId,int count,int selected,const wchar_t*const*names,float x,float y,float w,float h){
  const float gap=6.f;const float bw=(w-gap*(count-1))/count;
  for(int i=0;i<count;i++){int id=firstId+i;auto r=box(x+i*(bw+gap),y,bw,h);zone(id,Action,r);bool active=selected==i;
    auto c=active?accent:(s.hot==id?surface_hover:surface_active);
    s.r.fill_round(r,r_md,c);s.r.stroke_round(r,r_md,active?accent_hi:stroke_strong);
    s.r.text(names[i],r,FontCaption,active?accent_ink:text,AlignCenter);}
}
void dropdown(int id,const wchar_t*label,const wchar_t*const*items,int count,int selected,float x,float y,float w){
  auto r=box(x,y,w,38);zone(id,Action,r);bool open=s.openList==id;
  auto c=open?surface_active:(s.hot==id?surface_hover:surface_active);
  s.r.fill_round(r,r_md,c);s.r.stroke_round(r,r_md,open?accent:stroke_strong);
  s.r.text(label,box(x+14,y,w-56,38),FontCaption,text_hi);
  s.r.text(L"\u25BE",box(x+w-28,y,18,38),FontCaption,text_dim,AlignCenter);
  if(open){
    const float lh=32.f;const float listH=count*lh+10;auto lr=box(x,y-listH-6,w,listH);
    s.r.shadow(lr,r_md,8,.32f);s.r.fill_round(lr,r_md,elevated);s.r.stroke_round(lr,r_md,stroke_strong);
    for(int i=0;i<count;i++){auto ir=box(x,lr.top+5+i*lh,w,lh);zone(1000+i,Action,ir);bool active=selected==i;
      if(active)s.r.fill_round(ir,r_md,surface_active);else if(s.hot==1000+i)s.r.fill_round(ir,r_md,surface_hover);
      s.r.text(items[i],box(ir.left+14,ir.top,ir.right-ir.left-24,lh),FontCaption,active?accent:text);}
  }
}

const wchar_t* key_name(int vk){switch(vk){case VK_LBUTTON:return L"Mouse 1";case VK_RBUTTON:return L"Mouse 2";case VK_MBUTTON:return L"Mouse 3";case VK_XBUTTON1:return L"Mouse 4";case VK_XBUTTON2:return L"Mouse 5";case VK_SHIFT:return L"Shift";case VK_CONTROL:return L"Ctrl";case VK_MENU:return L"Alt";case VK_SPACE:return L"Space";default:break;}static wchar_t b[24];if(vk>=VK_F1&&vk<=VK_F12){swprintf_s(b,L"F%d",vk-VK_F1+1);return b;}if((vk>=L'0'&&vk<=L'9')||(vk>=L'A'&&vk<=L'Z')){b[0]=(wchar_t)vk;b[1]=0;return b;}swprintf_s(b,L"VK %d",vk);return b;}

void draw_rail(){
  auto size=s.r.size();s.r.fill(box(0,0,rail_w,size.height),dvm::theme::rail);
  s.r.gradient_round(box(18,18,42,42),12,accent,accent_hi);s.r.text(L"LKS",box(18,18,42,42),FontBodyStrong,accent_ink,AlignCenter);
  s.r.text(L"LKS667 KERNEL RO CS2",box(72,17,154,20),FontCaption,text_hi);s.r.text(L"CONTROL PANEL",box(72,39,145,16),FontMicro,accent);
  const wchar_t* names[]={tr(L"Visée",L"Aim"),L"ESP",tr(L"Divers",L"Misc"),tr(L"Configuration",L"Settings")};const wchar_t glyphs[]={0xE945,0xE7B3,0xE713,0xE8B7};
  for(int i=0;i<4;i++){int id=10+i;auto r=box(14,106+i*50,rail_w-28,42);zone(id,Nav,r);bool active=(int)s.page==i;
    if(active||s.hot==id)s.r.fill_round(r,r_md,active?surface_active:surface_hover);if(active)s.r.fill_round(box(r.left,r.top+8,3,26),2,accent);
    s.r.text(glyph(glyphs[i]),box(r.left+14,r.top,28,42),FontIcon,active?accent:text_dim,AlignCenter);s.r.text(names[i],box(r.left+50,r.top,r.right-r.left-58,42),FontBodyStrong,active?text_hi:text_dim);
  }
  auto quit=box(14,size.height-166,rail_w-28,38);zone(30,Action,quit);s.r.fill_round(quit,r_md,s.hot==30?alpha(danger,.18f):surface);s.r.stroke_round(quit,r_md,s.hot==30?danger:stroke);s.r.text(tr(L"FIN",L"END"),quit,FontBodyStrong,s.hot==30?danger:text_dim,AlignCenter);
  auto fr=box(16,size.height-114,50,36),en=box(72,size.height-114,50,36);zone(20,Nav,fr);zone(21,Nav,en);
  s.r.fill_round(fr,8,s.french?surface_active:surface);s.r.stroke_round(fr,8,s.french?accent:stroke,2);auto ff=box(fr.left+8,fr.top+8,34,20);s.r.fill(ff,hex(0xFFFFFF));s.r.fill(box(ff.left,ff.top,11.5f,20),hex(0x0055A4));s.r.fill(box(ff.right-11.5f,ff.top,11.5f,20),hex(0xEF4135));
  s.r.fill_round(en,8,!s.french?surface_active:surface);s.r.stroke_round(en,8,!s.french?accent:stroke,2);auto uf=box(en.left+8,en.top+8,34,20);s.r.fill(uf,hex(0x012169));
  auto p=[&](std::initializer_list<D2D1_POINT_2F> pts,D2D1_COLOR_F color){s.r.fill_polygon(std::vector<D2D1_POINT_2F>(pts),color);};float x=uf.left,y=uf.top;
  p({D2D1::Point2F(x,y),D2D1::Point2F(x+5,y),D2D1::Point2F(x+34,y+15),D2D1::Point2F(x+34,y+20),D2D1::Point2F(x+29,y+20),D2D1::Point2F(x,y+5)},hex(0xFFFFFF));
  p({D2D1::Point2F(x+29,y),D2D1::Point2F(x+34,y),D2D1::Point2F(x+34,y+5),D2D1::Point2F(x+5,y+20),D2D1::Point2F(x,y+20),D2D1::Point2F(x,y+15)},hex(0xFFFFFF));
  p({D2D1::Point2F(x,y),D2D1::Point2F(x+2.8f,y),D2D1::Point2F(x+34,y+18.3f),D2D1::Point2F(x+34,y+20),D2D1::Point2F(x+31.2f,y+20),D2D1::Point2F(x,y+1.7f)},hex(0xC8102E));
  p({D2D1::Point2F(x+31.2f,y),D2D1::Point2F(x+34,y),D2D1::Point2F(x+34,y+1.7f),D2D1::Point2F(x+2.8f,y+20),D2D1::Point2F(x,y+20),D2D1::Point2F(x,y+18.3f)},hex(0xC8102E));
  s.r.fill(box(x,y+7,34,6),hex(0xFFFFFF));s.r.fill(box(x+14,y,6,20),hex(0xFFFFFF));
  s.r.fill(box(x,y+8.5f,34,3),hex(0xC8102E));s.r.fill(box(x+15.5f,y,3,20),hex(0xC8102E));
  s.r.text(L"FR",box(120,size.height-108,28,27),FontMicro,s.french?accent:text_dim,AlignCenter);s.r.text(L"EN",box(150,size.height-108,28,27),FontMicro,!s.french?accent:text_dim,AlignCenter);
  s.r.text(L"F3",box(18,size.height-62,36,24),FontCaption,accent,AlignCenter);s.r.text(tr(L"Afficher / masquer",L"Show / hide"),box(62,size.height-64,150,24),FontCaption,text_dim);
}
void caption(){auto size=s.r.size();const wchar_t gs[]={0xE921,0xE8BB};for(int i=0;i<2;i++){int id=90+i;auto r=box(size.width-(2-i)*46,0,46,40);zone(id,Caption,r);if(s.hot==id)s.r.fill(r,id==91?danger:surface_hover);s.r.text(glyph(gs[i]),r,FontIcon,text_hi,AlignCenter);}}

void aim(float l,float top,float w){title(L"COMBAT",tr(L"Assistance à la visée",L"Aim assist"),tr(L"Réglages de ciblage et de comportement.",L"Targeting and behavior settings."));float gap=14,cw=(w-gap)/2;auto a=box(l,top,cw,350),b=box(l+cw+gap,top,cw,350);card(a);card(b);section(tr(L"Ciblage",L"Targeting"),a.left+20,a.top+18,cw-40);
 toggle(300,tr(L"Activer l'aim",L"Enable aim"),tr(L"Autorise le système existant",L"Enable the existing system"),g_AimSettings.enabled,a.left+12,a.top+50,cw-24);toggle(305,L"Team check",tr(L"Ignore les alliés",L"Ignore teammates"),g_AimSettings.teamCheck,a.left+12,a.top+112,cw-24);toggle(311,tr(L"Visibles uniquement",L"Visible targets only"),tr(L"Cibles avec ligne de vue",L"Targets in line of sight"),g_AimSettings.visibleOnly,a.left+12,a.top+174,cw-24);std::wstring keyText=g_ListeningForKey?tr(L"Appuyez sur une touche…",L"Press a key…"):(std::wstring(tr(L"Touche : ",L"Key: "))+key_name(g_AimSettings.aimKey));action(301,keyText.c_str(),a.left+20,a.top+252,cw-40,true);
 section(tr(L"Précision",L"Precision"),b.left+20,b.top+18,cw-40);slider(303,tr(L"Champ de vision",L"Field of view"),g_AimSettings.aimFov,1.f,30.f,L"°",b.left+12,b.top+54,cw-24);slider(304,tr(L"Lissage",L"Smoothness"),g_AimSettings.aimSmooth,1.f,20.f,L"",b.left+12,b.top+126,cw-24);
 const wchar_t* targets[]={tr(L"Tête",L"Head"),tr(L"Cou",L"Neck"),tr(L"Poitrine",L"Chest"),tr(L"Bassin",L"Pelvis")};int boneSel=g_AimSettings.aimBone==BONE_NECK?1:g_AimSettings.aimBone==BONE_CHEST?2:g_AimSettings.aimBone==BONE_PELVIS?3:0;section(tr(L"Cible",L"Target"),b.left+20,b.top+198,cw-40);seg(316,4,boneSel,targets,b.left+12,b.top+224,cw-24,38);
}
void esp(float l,float top,float w){title(tr(L"VISUELS",L"VISUALS"),L"ESP",tr(L"Informations claires sur les joueurs et le monde.",L"Clear player and world information."));float gap=12,c1=285,c2=220,c3=w-c1-c2-gap*2;auto a=box(l,top,c1,500),b=box(l+c1+gap,top,c2,500),c=box(l+c1+c2+gap*2,top,c3,500);card(a);card(b);card(c);section(tr(L"Joueurs",L"Players"),a.left+18,a.top+16,c1-36);
 struct T{int id;const wchar_t*n;const wchar_t*d;bool v;};std::array<T,8> ts={{{105,tr(L"Activer l'ESP",L"Enable ESP"),tr(L"Affichage principal",L"Main display"),g_EspSettings.enabled},{100,tr(L"Boîtes",L"Boxes"),tr(L"Cadre joueur",L"Player frame"),g_EspSettings.box},{101,tr(L"Squelette",L"Skeleton"),tr(L"Structure osseuse",L"Bone structure"),g_EspSettings.skeleton},{107,tr(L"Tête",L"Head"),tr(L"Repère de tête",L"Head marker"),g_EspSettings.headEsp},{102,tr(L"Santé",L"Health"),tr(L"Barre de vie",L"Health bar"),g_EspSettings.health},{113,tr(L"Bouclier",L"Shield"),tr(L"Barre d'armure",L"Armor bar"),g_EspSettings.shield},{103,tr(L"Nom du joueur",L"Player name"),tr(L"Affiche le nom",L"Show player name"),g_EspSettings.name},{104,tr(L"Visibles uniquement",L"Visible only"),tr(L"Filtre de visibilité",L"Visibility filter"),g_EspSettings.visibleOnly}}};for(int i=0;i<8;i++)toggle(ts[i].id,ts[i].n,ts[i].d,ts[i].v,a.left+8,a.top+42+i*54,c1-16);
 section(tr(L"Monde",L"World"),b.left+16,b.top+16,c2-32);toggle(109,tr(L"Armes",L"Weapons"),tr(L"Objets au sol",L"Ground items"),g_EspSettings.showWeapons,b.left+8,b.top+48,c2-16);toggle(110,L"Grenades",L"Projectiles",g_EspSettings.showGrenades,b.left+8,b.top+108,c2-16);toggle(111,tr(L"Bombe",L"Bomb"),tr(L"Objectif",L"Objective"),g_EspSettings.showBomb,b.left+8,b.top+168,c2-16);toggle(108,tr(L"Cercle FOV",L"FOV circle"),tr(L"Repère de visée",L"Aim guide"),g_EspSettings.showFovCircle,b.left+8,b.top+228,c2-16);toggle(112,tr(L"Poulets",L"Chickens"),L"Chicken ESP",g_EspSettings.showChickens,b.left+8,b.top+288,c2-16);
 section(tr(L"Couleurs",L"Colors"),c.left+16,c.top+16,c3-32);color_row(200,tr(L"Boîtes",L"Boxes"),g_EspSettings.boxColor,c.left+8,c.top+48,c3-16);color_row(201,tr(L"Squelette",L"Skeleton"),g_EspSettings.skeletonColor,c.left+8,c.top+102,c3-16);color_row(203,tr(L"Visible",L"Visible"),g_EspSettings.skeletonVisibleColor,c.left+8,c.top+156,c3-16);color_row(204,tr(L"Masqué",L"Hidden"),g_EspSettings.skeletonHiddenColor,c.left+8,c.top+210,c3-16);color_row(202,tr(L"Cercle FOV",L"FOV circle"),g_EspSettings.fovCircleColor,c.left+8,c.top+264,c3-16);
 const wchar_t* boxStyles[]={tr(L"Coin",L"Corner"),tr(L"Normal",L"Normal"),tr(L"3D",L"3D"),tr(L"3D Coin",L"3D Corner"),tr(L"Arrondi",L"Rounded"),tr(L"Plein",L"Filled"),tr(L"Cercle",L"Circle")};section(tr(L"Style de boîte",L"Box style"),c.left+16,c.top+330,c3-32);dropdown(327,boxStyles[std::clamp(g_EspSettings.boxStyle,0,BOX_STYLE_COUNT-1)],boxStyles,BOX_STYLE_COUNT,std::clamp(g_EspSettings.boxStyle,0,BOX_STYLE_COUNT-1),c.left+8,c.top+356,c3-16);
}
void misc(float l,float top,float w){title(tr(L"AFFICHAGE",L"DISPLAY"),tr(L"Options diverses",L"Miscellaneous"),tr(L"Informations complémentaires du moteur actuel.",L"Additional information from the existing engine."));auto c=box(l,top,std::min(w,680.f),250);card(c);section(tr(L"Interface en jeu",L"In-game interface"),c.left+20,c.top+18,c.right-c.left-40);toggle(402,L"Crosshair",tr(L"Repère central",L"Center marker"),g_MiscSettings.showCrosshair,c.left+12,c.top+52,c.right-c.left-24);toggle(403,tr(L"Informations / FPS",L"Information / FPS"),tr(L"État de la session",L"Session status"),g_MiscSettings.showGameInfo,c.left+12,c.top+112,c.right-c.left-24);toggle(404,tr(L"Timer bombe",L"Bomb timer"),tr(L"Compte à rebours",L"Countdown"),g_MiscSettings.showBombTimer,c.left+12,c.top+172,c.right-c.left-24);}
void config(float l,float top,float w){title(tr(L"SYSTÈME",L"SYSTEM"),tr(L"Configuration",L"Settings"),tr(L"Choisissez un profil disponible avant de le charger.",L"Choose an available profile before loading it."));s.configs=ModernConfigNames();auto c=box(l,top,std::min(w,790.f),420);card(c);float listW=360;section(tr(L"Profils disponibles",L"Available profiles"),c.left+20,c.top+18,listW-40);std::wstring selected=ModernSelectedConfig();if(s.configs.empty())s.r.text(tr(L"Aucune sauvegarde disponible",L"No saved profile available"),box(c.left+20,c.top+70,listW-40,28),FontBody,text_dim);for(size_t i=0;i<s.configs.size()&&i<7;i++){int id=600+(int)i;auto row=box(c.left+16,c.top+52+(float)i*46,listW-32,40);zone(id,Action,row);bool active=_wcsicmp(selected.c_str(),s.configs[i].c_str())==0;if(active||s.hot==id)s.r.fill_round(row,r_md,active?surface_active:surface_hover);if(active)s.r.fill_round(box(row.left,row.top+8,3,24),2,accent);s.r.text(s.configs[i],box(row.left+14,row.top,row.right-row.left-24,40),FontBodyStrong,active?text_hi:text);}
 float rx=c.left+listW+20;section(tr(L"Actions",L"Actions"),rx,c.top+18,c.right-rx-20);action(500,tr(L"Sauvegarder",L"Save"),rx,c.top+58,180,true);action(501,tr(L"Charger le profil",L"Load profile"),rx,c.top+112,180);action(502,tr(L"Valeurs par défaut",L"Reset defaults"),rx,c.top+184,180,false,true);action(503,tr(L"Vider le cache",L"Clear cache"),rx,c.top+238,180,false,true);}

const wchar_t* tip(int id){switch(id){
case 10:return tr(L"Ouvre les réglages de visée, de touche, de FOV et de lissage.",L"Open aim, key, FOV and smoothness settings.");case 11:return tr(L"Ouvre les informations visuelles des joueurs, du monde et leurs couleurs.",L"Open player and world visuals and their colors.");case 12:return tr(L"Ouvre les éléments d'affichage complémentaires en jeu.",L"Open additional in-game display elements.");case 13:return tr(L"Affiche les profils sauvegardés et les opérations de maintenance.",L"Show saved profiles and maintenance actions.");
case 20:return tr(L"Affiche immédiatement toute l'interface en français.",L"Display the whole interface in French immediately.");case 21:return tr(L"Affiche immédiatement toute l'interface en anglais.",L"Display the whole interface in English immediately.");
case 30:return tr(L"Ferme proprement le programme avec le fondu et le son de sortie.",L"Close the program cleanly with the exit fade and sound.");
case 100:return tr(L"Dessine un cadre autour de chaque joueur détecté.",L"Draw a frame around each detected player.");case 101:return tr(L"Relie les positions des os pour représenter le squelette du joueur.",L"Connect bone positions to display the player's skeleton.");case 102:return tr(L"Affiche une barre indiquant les points de vie restants.",L"Display a bar showing remaining health points.");case 103:return tr(L"Affiche le nom du joueur près de son cadre.",L"Display the player's name next to the frame.");case 104:return tr(L"Masque les joueurs qui ne sont pas directement visibles.",L"Hide players who are not directly visible.");case 105:return tr(L"Active ou coupe l'ensemble des éléments ESP.",L"Enable or disable all ESP elements.");case 107:return tr(L"Ajoute un repère visuel sur la position de la tête.",L"Add a visual marker at the head position.");case 108:return tr(L"Affiche à l'écran la zone couverte par le réglage FOV de visée.",L"Show the area covered by the aim FOV setting.");case 109:return tr(L"Affiche les armes détectées au sol.",L"Display detected weapons on the ground.");case 110:return tr(L"Affiche les grenades et projectiles détectés.",L"Display detected grenades and projectiles.");case 111:return tr(L"Affiche la position et les informations de l'objectif bombe.",L"Display the bomb objective position and information.");
case 112:return tr(L"Affiche un libellé jaune sur chaque poulet détecté dans la carte.",L"Display a yellow label on each chicken detected on the map.");case 113:return tr(L"Affiche la barre d'armure (bouclier) à droite du cadre.",L"Display the armor (shield) bar on the right side of the frame.");
case 200:return tr(L"Définit la couleur des cadres ESP.",L"Set the ESP frame color.");case 201:return tr(L"Définit la couleur générale du squelette.",L"Set the default skeleton color.");case 202:return tr(L"Définit la couleur du cercle représentant le FOV.",L"Set the FOV circle color.");case 203:return tr(L"Définit la couleur du squelette lorsque la cible est visible.",L"Set the skeleton color when the target is visible.");case 204:return tr(L"Définit la couleur du squelette lorsque la cible est masquée.",L"Set the skeleton color when the target is hidden.");
case 320:return tr(L"Coins 2D : quatre angles pour un cadrage discret.",L"2D corners: four corner marks for a discreet frame.");case 321:return tr(L"Rectangle 2D complet classique.",L"Classic full 2D rectangle.");case 322:return tr(L"Boîte 3D alignée sur le monde.",L"World-aligned 3D box.");case 323:return tr(L"Coins de la boîte 3D, cadrage léger.",L"3D box corners only, lightweight frame.");case 324:return tr(L"Rectangle 2D aux coins arrondis.",L"2D rectangle with rounded corners.");case 325:return tr(L"Rectangle 2D avec intérieur rempli sombre.",L"2D rectangle with a dark filled interior.");case 326:return tr(L"Cercle centré sur le corps du joueur.",L"Circle centered on the player's body.");case 327:return tr(L"Choisit le style de cadre des joueurs.",L"Choose the player frame style.");
case 300:return tr(L"Active ou coupe le système de visée configuré.",L"Enable or disable the configured aim system.");case 301:return tr(L"Cliquez, puis appuyez sur la touche ou le bouton de souris à utiliser.",L"Click, then press the key or mouse button to use.");case 303:return tr(L"Règle l'angle de recherche des cibles : faible = zone plus étroite.",L"Set the target search angle: lower means a narrower area.");case 304:return tr(L"Règle la progressivité du mouvement : élevé = déplacement plus doux.",L"Set movement interpolation: higher means smoother movement.");case 305:return tr(L"Empêche la sélection des joueurs appartenant à votre équipe.",L"Prevent players on your team from being selected.");case 311:return tr(L"Limite la sélection aux cibles directement visibles.",L"Restrict selection to directly visible targets.");
case 316:return tr(L"Vise le centre de la tête de la cible.",L"Aim at the center of the target's head.");case 317:return tr(L"Vise le cou de la cible.",L"Aim at the target's neck.");case 318:return tr(L"Vise le torse de la cible.",L"Aim at the target's chest.");case 319:return tr(L"Vise le bassin de la cible.",L"Aim at the target's pelvis.");
case 402:return tr(L"Affiche un repère fixe au centre de l'écran.",L"Display a fixed marker at the center of the screen.");case 403:return tr(L"Affiche les informations de session et le nombre d'images par seconde.",L"Display session information and frames per second.");case 404:return tr(L"Affiche le temps restant avant l'explosion de la bombe.",L"Display the remaining time before the bomb explodes.");
case 500:return tr(L"Enregistre tous les réglages dans le profil actuellement sélectionné.",L"Save all settings to the currently selected profile.");case 501:return tr(L"Remplace les réglages actuels par ceux du profil sélectionné.",L"Replace current settings with those from the selected profile.");case 502:return tr(L"Rétablit les valeurs d'origine sans supprimer les fichiers sauvegardés.",L"Restore original values without deleting saved files.");case 503:return tr(L"Supprime le cache afin de forcer une nouvelle détection à la prochaine connexion.",L"Delete the cache to force detection again on the next connection.");default:if(id>=600)return tr(L"Sélectionne ce fichier comme profil pour Charger ou Sauvegarder.",L"Select this file as the profile used by Load or Save.");if(id>=1000)return tr(L"Appliquer ce style de boîte aux joueurs.",L"Apply this box style to players.");return nullptr;}}
void tooltip(){const wchar_t*t=tip(s.hot);if(!t)return;auto sz=s.r.size();float w=510,textW=w-54;float textH=std::max(16.f,s.r.text_height(t,FontCaption,textW));float h=textH+14;auto r=box(sz.width-w-24,sz.height-52-h,w,h);s.r.shadow(r,r_md,6,.32f);s.r.fill_round(r,r_md,elevated);s.r.stroke_round(r,r_md,stroke_strong);s.r.fill_circle(r.left+20,r.top+h*.5f,8,info);s.r.text(L"i",box(r.left+15,r.top+(h-24)*.5f,10,24),FontCaption,text_hi,AlignCenter);s.r.paragraph(t,box(r.left+40,r.top+7,textW,textH),FontCaption,text_hi);}
void render(){if(!s.r.ready())return;s.zones.clear();s.r.begin();s.r.fill(box(0,0,s.r.size().width,s.r.size().height),app);draw_rail();caption();float l=rail_w+28,top=166,w=s.r.size().width-l-28;switch(s.page){case Page::Aim:aim(l,top,w);break;case Page::Esp:esp(l,top,w);break;case Page::Misc:misc(l,top,w);break;case Page::Config:config(l,top,w);break;}auto sz=s.r.size();s.r.fill(box(rail_w,sz.height-46,sz.width-rail_w,46),dvm::theme::rail);wchar_t status[300]{};if(g_hStatus)GetWindowTextW(g_hStatus,status,300);s.r.fill_circle(rail_w+30,sz.height-23,4,accent);s.r.text(status,box(rail_w+44,sz.height-34,sz.width-rail_w-60,24),FontCaption,text_dim);tooltip();if(!s.r.end())InvalidateRect(s.hwnd,nullptr,FALSE);}

bool* toggle_ptr(int id){switch(id){case 100:return &g_EspSettings.box;case 101:return &g_EspSettings.skeleton;case 102:return &g_EspSettings.health;case 113:return &g_EspSettings.shield;case 103:return &g_EspSettings.name;case 104:return &g_EspSettings.visibleOnly;case 105:return &g_EspSettings.enabled;case 107:return &g_EspSettings.headEsp;case 108:return &g_EspSettings.showFovCircle;case 109:return &g_EspSettings.showWeapons;case 110:return &g_EspSettings.showGrenades;case 111:return &g_EspSettings.showBomb;case 112:return &g_EspSettings.showChickens;case 300:return &g_AimSettings.enabled;case 305:return &g_AimSettings.teamCheck;case 311:return &g_AimSettings.visibleOnly;case 402:return &g_MiscSettings.showCrosshair;case 403:return &g_MiscSettings.showGameInfo;case 404:return &g_MiscSettings.showBombTimer;case 405:return &g_MiscSettings.showDamageLog;default:return nullptr;}}
void update_slider(int id,int x){const Zone*z=nullptr;for(auto&v:s.zones)if(v.id==id){z=&v;break;}if(!z)return;float left=z->rect.left+14,right=z->rect.right-18,t=std::clamp((x-left)/(right-left),0.f,1.f);float lo=1.f,hi=id==303?30.f:20.f;float value=std::round((lo+(hi-lo)*t)*10.f)/10.f;if(id==303)g_AimSettings.aimFov=value;else g_AimSettings.aimSmooth=value;}
void activate(int id,int x){if(id>=10&&id<=13){s.page=static_cast<Page>(id-10);s.openList=-1;return;}if(id==20||id==21){s.french=id==20;return;}if(id==30){PostMessageW(s.hwnd,WM_CLOSE,0,0);return;}if(id>=600){size_t i=(size_t)(id-600);if(i<s.configs.size())ModernSelectConfig(s.configs[i]);return;}if(auto p=toggle_ptr(id)){*p=!*p;if(id>=402&&id<=405)HudRefresh();return;}if(id>=316&&id<=319){static const int bones[]={BONE_HEAD,BONE_NECK,BONE_CHEST,BONE_PELVIS};g_AimSettings.aimBone=bones[id-316];return;}if(id>=1000&&id<1000+BOX_STYLE_COUNT){g_EspSettings.boxStyle=id-1000;s.openList=-1;return;}if(id==327){s.openList=s.openList==327?-1:327;return;}if(id==90){ShowWindow(s.hwnd,SW_MINIMIZE);return;}if(id==91){PostMessageW(s.hwnd,WM_CLOSE,0,0);return;}if(id==303||id==304){update_slider(id,x);return;}PostMessageW(s.hwnd,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),(LPARAM)s.hwnd);}
}

bool initialize(HWND w){s.hwnd=w;s.dpi=GetDpiForWindow(w);if(!s.r.create_device_independent())return false;RECT rc{};GetClientRect(w,&rc);return s.r.create_target(w,s.dpi)&& (s.r.resize(rc.right,rc.bottom),true);}
void shutdown(){s.r.destroy();s={};}
void resize(unsigned w,unsigned h){s.r.resize(w,h);InvalidateRect(s.hwnd,nullptr,FALSE);}
void paint(){render();}
void mouse_move(int x,int y){if(s.down==303||s.down==304)update_slider(s.down,x);int h=hit((float)x,(float)y);if(h!=s.hot||s.down==303||s.down==304){s.hot=h;InvalidateRect(s.hwnd,nullptr,FALSE);}TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,s.hwnd,0};TrackMouseEvent(&t);}
void mouse_leave(){s.hot=-1;InvalidateRect(s.hwnd,nullptr,FALSE);}
void mouse_down(int x,int y){int h=hit((float)x,(float)y);s.down=h;if(s.openList==327&&h!=327&&(h<1000||h>=1000+BOX_STYLE_COUNT))s.openList=-1;if(s.down==303||s.down==304)update_slider(s.down,x);SetCapture(s.hwnd);InvalidateRect(s.hwnd,nullptr,FALSE);}
void mouse_up(int x,int y){int id=hit((float)x,(float)y);ReleaseCapture();if(id==s.down&&id>=0)activate(id,x);s.down=-1;InvalidateRect(s.hwnd,nullptr,FALSE);}
void wheel(short){ }
bool hit_caption_button(int x,int y){int id=hit((float)x,(float)y);return id==90||id==91;}
}
