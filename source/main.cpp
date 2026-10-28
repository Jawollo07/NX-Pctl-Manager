// NX-Pctl-Manager — Plutonium UI entry point.
// Copyright (C) 2026 Taylor. GPLv3-or-later; see LICENSE.
#include <pu/Plutonium>
#include <switch.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "app.hpp"
#include "clock_ops.h"
#include "remote_control.hpp"
#include "action/pt_flow.hpp"
#include "util/pctl_ops_c.hpp"

namespace {
const std::vector<std::string> Items = {
 "Refresh status", "Set / change parental-control PIN",
 "Set daily limit (all days)", "Set per-day limit", "Remove play-time limit",
 "Bedtime alarm (21:00–06:00 preset)", "When time is up",
 "Set network clock from system time", "Synchronize central server",
 "Delete all parental controls", "Unlink companion app", "Save diagnostic report", "Exit"
};
std::string status_text() {
 if (!app::pctl_available()) return "pctl unavailable. Check nx_pctl_manager.log.";
 PctlStatus s{}; pctl_status_fetch(&s); PtState pt{}; pctl_play_timer_query(&pt);
 ClockStatus c{}; clock_status(&c);
 std::string out="Restrictions: ";
 out += s.restriction_enabled_ok ? (s.restriction_enabled?"enabled":"disabled") : "unavailable";
 out += " | Safety: "; out += s.safety_level_ok?pctl_safety_level_name(s.safety_level):"unavailable";
 out += " | PIN: "; out += s.pin_length_ok?(s.pin_length?std::to_string(s.pin_length)+" digits":"not set"):"unavailable";
 out += "\nPlay timer: "; out += !pt.supported?"unsupported":(!pt.valid?"unavailable":(pt.enabled?"on":"off"));
 out += " | Network clock: "; out += c.network_ok?"available":"not set";
 return out;
}
class MainLayout final : public pu::ui::Layout {
public:
 pu::ui::elm::TextBlock::Ref title, status, menu, help;
 MainLayout() {
  title=pu::ui::elm::TextBlock::New(42,24,"NX-Pctl-Manager");
  status=pu::ui::elm::TextBlock::New(42,78,"Starting...");
  menu=pu::ui::elm::TextBlock::New(62,170,"");
  help=pu::ui::elm::TextBlock::New(42,660,"D-pad: select    A: open    B / +: exit");
  Add(title); Add(status); Add(menu); Add(help);
 }
 PU_SMART_CTOR(MainLayout)
};
class MainApp final : public pu::ui::Application {
 MainLayout::Ref ui; std::size_t selected=0; bool ready=false, authenticated=false;
public:
 using Application::Application;
 PU_SMART_CTOR(MainApp)
 void OnLoad() override {
  ui=MainLayout::New(); LoadLayout(ui);
  SetOnInput([this](const u64 down,const u64,const u64,const pu::ui::TouchPoint) {
   if (down & (HidNpadButton_Plus|HidNpadButton_B)) ExitApp();
   else if (down & HidNpadButton_Up) { if(selected) --selected; DrawMenu(); }
   else if (down & HidNpadButton_Down) { if(selected+1<Items.size()) ++selected; DrawMenu(); }
   else if (down & HidNpadButton_A) Run();
  });
  ready=app::init();
  ui->status->SetText(ready?status_text():"pctl initialization failed. Press A to retry.");
  DrawMenu();
 }
private:
 void ExitApp(){ if(ready) app::shutdown(); Close(true); }
 void DrawMenu(){
  std::string text; const std::size_t first=selected>7?selected-7:0;
  for(std::size_t i=first;i<std::min(Items.size(),first+10);++i)
   text += (i==selected?"> ":"  ")+Items[i]+"\n";
  ui->menu->SetText(text);
 }
 void Refresh(){ if(!ready) ready=app::init(); ui->status->SetText(ready?status_text():"pctl init failed; see log."); }
 s32 Dialog(const std::string& t,const std::string& b,const std::vector<std::string>& opts){ return CreateShowDialog(t,b,opts,true); }
 bool Confirm(const std::string& t,const std::string& b){ return Dialog(t,b,{"Cancel","Confirm"})==1; }
 void ResultDialog(const std::string& title,Result rc){
  Dialog(title,R_SUCCEEDED(rc)?"Operation completed.":"Operation failed (result "+std::to_string((unsigned)rc)+"). Check nx_pctl_manager.log.",{"OK"}); Refresh();
 }
 void Run(){
  if(!ready){ Refresh(); return; }
  switch(selected){
   case 0: Refresh(); break;
   case 1: ResultDialog("Set / change PIN",pctl_set_pin()); break;
   case 2: UniformLimit(); break;
   case 3: PerDayLimit(); break;
   case 4:
    if(Confirm("Remove play-time limit","Remove all daily limits? Any bedtime alarm is preserved."))
     pt_flow::guarded_write([]{return pctl_play_timer_clear();},"Play-time limit removed.");
    Refresh(); break;
   case 5: Bedtime(); break;
   case 6: Mode(); break;
   case 7: SeedClock(); break;
   case 8: Sync(); break;
   case 9:
    if(Confirm("Delete all parental controls","IRREVERSIBLE: removes the PIN and every restriction. Continue?"))
     ResultDialog("Delete parental controls",pctl_delete_parental_controls());
    break;
   case 10:
    if(Confirm("Unlink companion app","Remove pairing with the Nintendo Switch Parental Controls phone app?"))
     ResultDialog("Unlink companion app",pctl_delete_pairing());
    break;
   case 11: Diagnostics(); break;
   case 12: ExitApp(); break;
  }
  DrawMenu();
 }
 void UniformLimit(){
  s32 i=Dialog("Daily limit","Choose minutes for every day. 0 means block play.",{"30","60","90","120","180","0 (block)","Cancel"});
  const u16 vals[]={30,60,90,120,180,0}; if(i<0||i>5)return;
  bool ok=pt_flow::guarded_write([v=vals[i]]{return pctl_play_timer_set_uniform(v);},"Daily limit saved.");
  Dialog("Daily limit",ok?"Saved.":"Write failed; check PIN, firmware support and log.",{"OK"}); Refresh();
 }
 void PerDayLimit(){
  s32 d=Dialog("Per-day limit","Choose a day. This change is applied immediately.",{"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Cancel"});
  if(d<0||d>6)return;
  s32 i=Dialog("Per-day limit","0 blocks play; No limit leaves this day unrestricted.",{"30","60","90","120","180","0 (block)","No limit","Cancel"});
  if(i<0||i>6)return;
  PtState pt{}; pctl_play_timer_query(&pt);
  if(!pt.valid){Dialog("Per-day limit","Could not read settings; nothing changed.",{"OK"});return;}
  u16 days[7]; for(int n=0;n<7;++n)days[n]=pt.day_min[n];
  const u16 vals[]={30,60,90,120,180,0,PT_DAY_NOLIMIT}; days[d]=vals[i];
  bool ok=pt_flow::guarded_write([days]{return pctl_play_timer_set_days(days);},"Per-day limit saved.");
  Dialog("Per-day limit",ok?"Saved.":"Write failed; check PIN, firmware support and log.",{"OK"}); Refresh();
 }
 void Bedtime(){
  s32 i=Dialog("Bedtime alarm","Preset only: bedtime 21:00, reset 06:00.",{"Enable preset","Turn off","Cancel"});
  if(i==0||i==1){ PtBedtime b={i==0,21,0,6,0};
   bool ok=pt_flow::guarded_write([b]{return pctl_play_timer_set_bedtime(&b);},"Bedtime updated.");
   Dialog("Bedtime alarm",ok?"Updated.":"Write failed; check PIN, firmware support and log.",{"OK"}); Refresh(); }
 }
 void Mode(){
  s32 i=Dialog("When time is up","Choose the action when the limit is reached.",{"Suspend software","Notify only","Cancel"});
  if(i==0||i==1){u8 m=i==0?PT_MODE_SUSPEND:PT_MODE_ALARM;
   bool ok=pt_flow::guarded_write([m]{return pctl_play_timer_set_mode(m);},"Play-timer mode saved.");
   Dialog("Play timer",ok?"Mode saved.":"Could not save mode; configure a limit first.",{"OK"});Refresh();}
 }
 void SeedClock(){
  ClockStatus c{}; clock_status(&c);
  if(c.network_ok&&!Confirm("Network clock","Network clock is already set. Replace it with system time?"))return;
  ResultDialog("Set network clock",clock_seed_network_from_user());
 }
 void Sync(){
  std::string message;
  if(!authenticated){authenticated=remote_control::authenticate(message);if(!authenticated){Dialog("Central control",message,{"OK"});return;}}
  const auto r=remote_control::synchronize(); Dialog("Central control",r.message,{"OK"}); Refresh();
 }
 void Diagnostics(){
  static char buf[8192]; pctl_play_timer_dump(buf,sizeof(buf));
  FILE* f=std::fopen("/switch/nx_pctl_dump.txt","w"); bool ok=f&&std::fputs(buf,f)>=0; if(f)std::fclose(f);
  Dialog("Diagnostics",ok?"Saved to /switch/nx_pctl_dump.txt.":"Could not write diagnostic report.",{"OK"});
 }
};
}
int main(){
 auto options=pu::ui::render::RendererInitOptions(SDL_INIT_EVERYTHING,pu::ui::render::RendererHardwareFlags);
 options.UseImage(pu::ui::render::ImgAllFlags); options.SetPlServiceType(PlServiceType_User);
 options.AddDefaultAllSharedFonts(); options.SetInputPlayerCount(1);
 options.AddInputNpadStyleTag(HidNpadStyleSet_NpadStandard);
 options.AddInputNpadIdType(HidNpadIdType_Handheld); options.AddInputNpadIdType(HidNpadIdType_No1);
 auto renderer=pu::ui::render::Renderer::New(options); auto application=MainApp::New(renderer);
 const auto rc=application->Load(); if(R_FAILED(rc))diagAbortWithResult(rc);
 application->Show(); return 0;
}
