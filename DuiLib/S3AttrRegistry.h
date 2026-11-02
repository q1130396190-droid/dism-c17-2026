#ifndef __S3_ATTR_REGISTRY_H__
#define __S3_ATTR_REGISTRY_H__

// Auto-generated: S3 attribute registry. DO NOT EDIT.
// Regenerate with: python tools/gen_s3_registry.py --emit
//
// Design notes:
//   * Two separate tables. Table A = SetAttribute pstrName attributes;
//     Table B = inline DSL subkeys (inside image="file='x' corner='1,1,1,1'").
//     Merging them would make the N1 validator accept a DSL subkey as a
//     standalone XML attribute, which is wrong.
//   * FNV-1a 64 + lowercase normalization => matches _tcsicmp semantics and
//     also unifies the 45 _tcscmp sites found in round 4.
//   * Sorted by hash; runtime lookup is binary search, O(log n).
//   * Miss returns Unknown; callers MUST fall back to the legacy string
//     compare so third-party custom attributes keep working.
//
// NOTE: keep this header pure ASCII. It is included from translation units
// compiled under code page 936 (GBK); non-ASCII bytes here cause C4819 and
// spurious syntax errors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace DuiLib {
namespace s3 {

// ---- constexpr FNV-1a 64 (lowercase-normalized) ----
// All attribute names are ASCII, so masking a wchar_t to its low byte yields
// the same hash as the narrow-char form. This avoids runtime conversion and
// avoids generating a second, wide-char table.
constexpr std::uint64_t hash_attr(std::string_view s) noexcept
{
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001B3ull;
    }
    return h;
}

// TCHAR variant: identical result to the narrow-char form (names are ASCII)
template <class CharT>
constexpr std::uint64_t hash_attr_t(const CharT* s) noexcept
{
    std::uint64_t h = 0xCBF29CE484222325ull;
    if (s == nullptr) return h;
    for (const CharT* p = s; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p & 0xFF);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        h ^= static_cast<std::uint64_t>(c);
        h *= 0x100000001B3ull;
    }
    return h;
}

// Runtime lookup entry: accepts a TCHAR pointer directly (LPCTSTR on Windows)
template <class CharT>
constexpr inline std::uint64_t hash_ptr(const CharT* s) noexcept
{
    return hash_attr_t(s);
}

// ---- Table A: pstrName attributes of SetAttribute ----
enum class PropId : std::int32_t { Unknown = -1, thumbsize, itemdisabledtextcolor, padding, bindtabindex, noactivate, dropbox, selectedid, animation_direction, editable, selected, path, checkboxnormalimage, mousechild, selectedhotimage, ListContainerHeaderItem, layeredimage, min, shared, max, autovscroll, itemhottextcolor, itemtextcolor, expland, selitemhottextcolor, barheight, mouse, foreimage, focusedtextcolor, borderround, childalign, inset, nativebkcolor, pagebkcolor, checkboxpushedimage, height, autohscroll, richevent, selectedcolor, sepimage, time, mask, button1hotimage, checkboxselectedimage, hotforeimage, hscrollbarstyle, itemhotbkcolor, showdirty, selectedbkcolor, multiselect, endellipsis, childpadding, autoplay, sepwidth, autosize, numberonly, itemselectedbkcolor, disabledimage, tooltiphovertime, disabledbkcolor, hor, defaultfontcolor, button2pushedimage, webview2, clsid, linkfontcolor, gdiplustext, header, showshadow, hottextcolor, vscrollbar, checkable, layered, showscrollbar, hotimage, focusedimage, comboable, pushedforeimage, menu, selectedforeimage, leftbordersize, opacity, thumbpushedimage, checkboxhotimage, rightbordersize, dragable, thumbimage, dropboxsize, password, bkdisabledimage, shadowsharpness, size, passwordchar, align, grouptype, range, itemtextpadding, innerradius, spoke, bordersize, folderattr, itemselectedimage, linkhoverfontcolor, userdata, sepimm, restype, railhotimage, nativetextcolor, horizattr, itemselectedtextcolor, shadowdarkness, sendmove, selectedtextcolor, caption, attr_auto, fixedscrollbar, dotlineattr, sepheight, tipvaluealign, hscrollbar, forecolor, bkpushedimage, selectedstateimage, shadowposition, headerbkimage, focusbordercolor, pushedbordercolor, EnableAutoCheck, linepadding, bknormalimage, xmlfile, maxchar, autoselall, pushedfont, wantctrlreturn, noprefix, bkimage, modulename, italic, focuedfont, scrollselect, scale, enabled, GroupBox, tipvalue, disabledtextcolor, hotbordercolor, arrowimage, source, pos, ListTextExElement, minheight, color, pageselectedbkcolor, tooltip, linesize, resourcetext, minwidth, autocalcheight, value, itemfont, wantreturn, button1pushedimage, textpadding, pushedtextcolor, floatalign, itemrselected, shortcut, defaultexpand, columns, itemiconimgsize, valign, sizebox, itemshowcolumnline, alpha, button1disabledimage, autonavi, itemforeimage, itemendellipsis, multipleitem, HotKey, borderstyle, selectedimage, shadowimage, itemvalign, selhotitemtextcolor, itemshowrowline, dropboxinset, topbordersize, selectedfont, pushedbkcolor, editmaxchar, textrenderinghint, multiline, bktrans, checkboxdisabledimage, readonly, railnormalimage, showhtml, attr_float, bordercolor, visiblecheckbtn, wordbreak, itemsize, shadowsize, itemhotimage, fontfile, button2hotimage, mininfo, normalimage, linecolor, FadeButton, step, isstretchfore, itemminwidth, button2disabledimage, itemdisabledbkcolor, count, rich, selitemtextcolor, underline, homepage, itemlinecolor, outradius, texthotcolor, gotoeditbordersize, thumbdisabledimage, roundcorner, thumbnormalimage, hotbkcolor, rhaa, disabledbordercolor, transparent, autocalcwidth, style, visiblefolderbtn, raildisabledimage, keyboard, ischeck, pushedimage, showbutton1, showbutton2, hotfont, name, scrollstepsize, itembkcolor, itemhotforeimage, checkboxheight, button1normalimage, multiexpanding, maxheight, button2normalimage, textcolor, bottombordersize, bold, tipvaluecolor, bindtablayoutname, itembkimage, showtext, checkboxwidth, checkitem, checkboxforeimage, linetype, expandable, bordervisible, maxpage, bkcolor, itemdisabledimage, thickness, width, icon, group, ListEx, selectedpushedimage, font, layeredopacity, pageselectedtextcolor, RollText, childvalign, pagehotbkcolor, delaycreate, scaleheader, thumbhotimage, bkcolor1, bkcolor3, bkcolor2, checkboxattr, maxinfo, bkhotimage, gradient, innerstyle, itemshowhtml, statecount, virtualwnd, shadowcolor, visible, vscrollbarstyle, itemattr, disabledfontcolor, shadowcorner, drag, drop, itemcheckimgsize, itemselectedforeimage, iconsize, colorhsl, maxwidth, palletheight, itemaltbk, strikeout, selectedstatecount, pagetextcolor, pagehottextcolor, cursor, text, wanttab, checkboxfocusedimage, railpushedimage, itemalign, stateimage, gotoeditbordercolor };
struct PropEntry { std::uint64_t hash; PropId id; };

inline constexpr PropEntry kPropTable[] = {
    PropEntry{ 3584875459551490ull, PropId::thumbsize },
    PropEntry{ 41910511850235664ull, PropId::itemdisabledtextcolor },
    PropEntry{ 105616063399437942ull, PropId::padding },
    PropEntry{ 138246584712883891ull, PropId::bindtabindex },
    PropEntry{ 150652247014434299ull, PropId::noactivate },
    PropEntry{ 167194788679118981ull, PropId::dropbox },
    PropEntry{ 174016884497616345ull, PropId::selectedid },
    PropEntry{ 206063115537040123ull, PropId::animation_direction },
    PropEntry{ 234255163322562769ull, PropId::editable },
    PropEntry{ 271429125968069076ull, PropId::selected },
    PropEntry{ 271672890340345462ull, PropId::path },
    PropEntry{ 287510971744611456ull, PropId::checkboxnormalimage },
    PropEntry{ 303301846174730066ull, PropId::mousechild },
    PropEntry{ 440082996573666676ull, PropId::selectedhotimage },
    PropEntry{ 448752481365816890ull, PropId::ListContainerHeaderItem },
    PropEntry{ 473741592819813068ull, PropId::layeredimage },
    PropEntry{ 573156827627667575ull, PropId::min },
    PropEntry{ 579278177429913076ull, PropId::shared },
    PropEntry{ 580772045163143161ull, PropId::max },
    PropEntry{ 719001289651078545ull, PropId::autovscroll },
    PropEntry{ 751861835194220149ull, PropId::itemhottextcolor },
    PropEntry{ 792148476886998312ull, PropId::itemtextcolor },
    PropEntry{ 852365639909133005ull, PropId::expland },
    PropEntry{ 853094214496280391ull, PropId::selitemhottextcolor },
    PropEntry{ 957317839182360569ull, PropId::barheight },
    PropEntry{ 965581846955831140ull, PropId::mouse },
    PropEntry{ 1092585877858796564ull, PropId::foreimage },
    PropEntry{ 1201554484768224956ull, PropId::focusedtextcolor },
    PropEntry{ 1279323954799073829ull, PropId::borderround },
    PropEntry{ 1300873171464148470ull, PropId::childalign },
    PropEntry{ 1442972188048846310ull, PropId::inset },
    PropEntry{ 1514131209529343692ull, PropId::nativebkcolor },
    PropEntry{ 1537354368118435592ull, PropId::pagebkcolor },
    PropEntry{ 1584740621692018530ull, PropId::checkboxpushedimage },
    PropEntry{ 1689425963507806754ull, PropId::height },
    PropEntry{ 1690355894887222179ull, PropId::autohscroll },
    PropEntry{ 1747729259157450021ull, PropId::richevent },
    PropEntry{ 1778473397470861083ull, PropId::selectedcolor },
    PropEntry{ 1854049831286908980ull, PropId::sepimage },
    PropEntry{ 2185518981507421060ull, PropId::time },
    PropEntry{ 2249371691748366409ull, PropId::mask },
    PropEntry{ 2423907274272054760ull, PropId::button1hotimage },
    PropEntry{ 2424047912469728518ull, PropId::checkboxselectedimage },
    PropEntry{ 2501291577866292831ull, PropId::hotforeimage },
    PropEntry{ 2513373653422578112ull, PropId::hscrollbarstyle },
    PropEntry{ 2531849698055103849ull, PropId::itemhotbkcolor },
    PropEntry{ 2576300998106429236ull, PropId::showdirty },
    PropEntry{ 2591900416271384522ull, PropId::selectedbkcolor },
    PropEntry{ 2640483852999942890ull, PropId::multiselect },
    PropEntry{ 2686883640562880197ull, PropId::endellipsis },
    PropEntry{ 2949283401849805950ull, PropId::childpadding },
    PropEntry{ 3132881932252203336ull, PropId::autoplay },
    PropEntry{ 3144577708494600161ull, PropId::sepwidth },
    PropEntry{ 3193338552135113911ull, PropId::autosize },
    PropEntry{ 3261159729119603346ull, PropId::numberonly },
    PropEntry{ 3412113058772809163ull, PropId::itemselectedbkcolor },
    PropEntry{ 3528553828148513690ull, PropId::disabledimage },
    PropEntry{ 3549997842534931619ull, PropId::tooltiphovertime },
    PropEntry{ 3595539223505248765ull, PropId::disabledbkcolor },
    PropEntry{ 3701714910296001902ull, PropId::hor },
    PropEntry{ 3758020719733621998ull, PropId::defaultfontcolor },
    PropEntry{ 3783846434841584365ull, PropId::button2pushedimage },
    PropEntry{ 3796226776116106330ull, PropId::webview2 },
    PropEntry{ 3867572372398192710ull, PropId::clsid },
    PropEntry{ 4106112261319557115ull, PropId::linkfontcolor },
    PropEntry{ 4144844409550036902ull, PropId::gdiplustext },
    PropEntry{ 4201977977006070112ull, PropId::header },
    PropEntry{ 4240137774264956248ull, PropId::showshadow },
    PropEntry{ 4349240280821472514ull, PropId::hottextcolor },
    PropEntry{ 4377761934879395885ull, PropId::vscrollbar },
    PropEntry{ 4480920979562037763ull, PropId::checkable },
    PropEntry{ 4580387886741056087ull, PropId::layered },
    PropEntry{ 4652237949479189258ull, PropId::showscrollbar },
    PropEntry{ 4728102891308652001ull, PropId::hotimage },
    PropEntry{ 4791028193456352599ull, PropId::focusedimage },
    PropEntry{ 4796687570512869121ull, PropId::comboable },
    PropEntry{ 4826035482039089071ull, PropId::pushedforeimage },
    PropEntry{ 4827579124670394010ull, PropId::menu },
    PropEntry{ 4842422824367141215ull, PropId::selectedforeimage },
    PropEntry{ 4856777441016367887ull, PropId::leftbordersize },
    PropEntry{ 4882808366398590118ull, PropId::opacity },
    PropEntry{ 4886500297109992603ull, PropId::thumbpushedimage },
    PropEntry{ 5174350902300227488ull, PropId::checkboxhotimage },
    PropEntry{ 5218037212726762478ull, PropId::rightbordersize },
    PropEntry{ 5249895935818563679ull, PropId::dragable },
    PropEntry{ 5343562185590187768ull, PropId::thumbimage },
    PropEntry{ 5370743588770056092ull, PropId::dropboxsize },
    PropEntry{ 5411718394350379800ull, PropId::password },
    PropEntry{ 5483432404869547175ull, PropId::bkdisabledimage },
    PropEntry{ 5593840529577384684ull, PropId::shadowsharpness },
    PropEntry{ 5614464919154503228ull, PropId::size },
    PropEntry{ 5645536630449868260ull, PropId::passwordchar },
    PropEntry{ 5807677297870517278ull, PropId::align },
    PropEntry{ 5917177872692597316ull, PropId::grouptype },
    PropEntry{ 5939887344697826482ull, PropId::range },
    PropEntry{ 6006647957491228710ull, PropId::itemtextpadding },
    PropEntry{ 6119989458346950741ull, PropId::innerradius },
    PropEntry{ 6156808314579536847ull, PropId::spoke },
    PropEntry{ 6243379959505372430ull, PropId::bordersize },
    PropEntry{ 6254934686578272682ull, PropId::folderattr },
    PropEntry{ 6257443407773627748ull, PropId::itemselectedimage },
    PropEntry{ 6295133882376585603ull, PropId::linkhoverfontcolor },
    PropEntry{ 6296213149510762914ull, PropId::userdata },
    PropEntry{ 6343970350258970240ull, PropId::sepimm },
    PropEntry{ 6463194505964769969ull, PropId::restype },
    PropEntry{ 6493441207808458573ull, PropId::railhotimage },
    PropEntry{ 6515080706286354468ull, PropId::nativetextcolor },
    PropEntry{ 6564089555884418906ull, PropId::horizattr },
    PropEntry{ 6569697737883936223ull, PropId::itemselectedtextcolor },
    PropEntry{ 6580377816005772142ull, PropId::shadowdarkness },
    PropEntry{ 6590671521392058726ull, PropId::sendmove },
    PropEntry{ 6690371527322320898ull, PropId::selectedtextcolor },
    PropEntry{ 6749251983603663525ull, PropId::caption },
    PropEntry{ 6757501175350325814ull, PropId::attr_auto },
    PropEntry{ 6787726525050349149ull, PropId::fixedscrollbar },
    PropEntry{ 6792283109968427241ull, PropId::dotlineattr },
    PropEntry{ 6826605286991410224ull, PropId::sepheight },
    PropEntry{ 6922483722362531416ull, PropId::tipvaluealign },
    PropEntry{ 7058694158107440543ull, PropId::hscrollbar },
    PropEntry{ 7103203237107529758ull, PropId::forecolor },
    PropEntry{ 7122889630240585968ull, PropId::bkpushedimage },
    PropEntry{ 7177461083128202702ull, PropId::selectedstateimage },
    PropEntry{ 7185108585495425734ull, PropId::shadowposition },
    PropEntry{ 7207429334239150476ull, PropId::headerbkimage },
    PropEntry{ 7220488552015976952ull, PropId::focusbordercolor },
    PropEntry{ 7236212117924781789ull, PropId::pushedbordercolor },
    PropEntry{ 7261745247546792179ull, PropId::EnableAutoCheck },
    PropEntry{ 7275733061576706772ull, PropId::linepadding },
    PropEntry{ 7313536787353077246ull, PropId::bknormalimage },
    PropEntry{ 7325835766564230224ull, PropId::xmlfile },
    PropEntry{ 7346764555106875969ull, PropId::maxchar },
    PropEntry{ 7361278619202427205ull, PropId::autoselall },
    PropEntry{ 7393419510447170705ull, PropId::pushedfont },
    PropEntry{ 7456153332071408152ull, PropId::wantctrlreturn },
    PropEntry{ 7476968535305398334ull, PropId::noprefix },
    PropEntry{ 7483332501424969407ull, PropId::bkimage },
    PropEntry{ 7570797836299646494ull, PropId::modulename },
    PropEntry{ 7626254364450527533ull, PropId::italic },
    PropEntry{ 7635072577205760394ull, PropId::focuedfont },
    PropEntry{ 7664952434021036020ull, PropId::scrollselect },
    PropEntry{ 7686723154774465937ull, PropId::scale },
    PropEntry{ 8131395913820547230ull, PropId::enabled },
    PropEntry{ 8307421968499603429ull, PropId::GroupBox },
    PropEntry{ 8333106260303412563ull, PropId::tipvalue },
    PropEntry{ 8339356973399190593ull, PropId::disabledtextcolor },
    PropEntry{ 8359654762403416717ull, PropId::hotbordercolor },
    PropEntry{ 8454917838996209867ull, PropId::arrowimage },
    PropEntry{ 8564681157369146808ull, PropId::source },
    PropEntry{ 8618582476283917417ull, PropId::pos },
    PropEntry{ 8619299534661685209ull, PropId::ListTextExElement },
    PropEntry{ 8619506461709036520ull, PropId::minheight },
    PropEntry{ 8644027876048135736ull, PropId::color },
    PropEntry{ 8697172863271078267ull, PropId::pageselectedbkcolor },
    PropEntry{ 8737837319773627664ull, PropId::tooltip },
    PropEntry{ 8807954189063023774ull, PropId::linesize },
    PropEntry{ 8826485773755420910ull, PropId::resourcetext },
    PropEntry{ 8837789411165233017ull, PropId::minwidth },
    PropEntry{ 8974643501521000268ull, PropId::autocalcheight },
    PropEntry{ 8999596768310594794ull, PropId::value },
    PropEntry{ 9103052745857729159ull, PropId::itemfont },
    PropEntry{ 9170321275105728077ull, PropId::wantreturn },
    PropEntry{ 9193919609775574538ull, PropId::button1pushedimage },
    PropEntry{ 9207954270449569263ull, PropId::textpadding },
    PropEntry{ 9221157474518348274ull, PropId::pushedtextcolor },
    PropEntry{ 9251977718534269278ull, PropId::floatalign },
    PropEntry{ 9516305762657623125ull, PropId::itemrselected },
    PropEntry{ 9547867214735548211ull, PropId::shortcut },
    PropEntry{ 9563010325542792570ull, PropId::defaultexpand },
    PropEntry{ 9663440624238318748ull, PropId::columns },
    PropEntry{ 9680130298728043011ull, PropId::itemiconimgsize },
    PropEntry{ 9685961658365068034ull, PropId::valign },
    PropEntry{ 9862757757988183189ull, PropId::sizebox },
    PropEntry{ 9988292341533439787ull, PropId::itemshowcolumnline },
    PropEntry{ 9999721509958787115ull, PropId::alpha },
    PropEntry{ 10024271523492407509ull, PropId::button1disabledimage },
    PropEntry{ 10027336168772829610ull, PropId::autonavi },
    PropEntry{ 10279241778889486421ull, PropId::itemforeimage },
    PropEntry{ 10306727697777062536ull, PropId::itemendellipsis },
    PropEntry{ 10379818272963107162ull, PropId::multipleitem },
    PropEntry{ 10407898433174093939ull, PropId::HotKey },
    PropEntry{ 10431257201642823560ull, PropId::borderstyle },
    PropEntry{ 10447068260834642145ull, PropId::selectedimage },
    PropEntry{ 10493830291240523830ull, PropId::shadowimage },
    PropEntry{ 10517150121069107581ull, PropId::itemvalign },
    PropEntry{ 10589057923965200045ull, PropId::selhotitemtextcolor },
    PropEntry{ 10593829225999186955ull, PropId::itemshowrowline },
    PropEntry{ 10705542329267796166ull, PropId::dropboxinset },
    PropEntry{ 10744401631572406235ull, PropId::topbordersize },
    PropEntry{ 11054663788313276961ull, PropId::selectedfont },
    PropEntry{ 11061684221535627354ull, PropId::pushedbkcolor },
    PropEntry{ 11083375719948380397ull, PropId::editmaxchar },
    PropEntry{ 11212148810462900203ull, PropId::textrenderinghint },
    PropEntry{ 11238278495084644704ull, PropId::multiline },
    PropEntry{ 11269638386911030548ull, PropId::bktrans },
    PropEntry{ 11273366792110826013ull, PropId::checkboxdisabledimage },
    PropEntry{ 11389206710689427095ull, PropId::readonly },
    PropEntry{ 11465835251546901227ull, PropId::railnormalimage },
    PropEntry{ 11477863712663702841ull, PropId::showhtml },
    PropEntry{ 11532138274943533413ull, PropId::attr_float },
    PropEntry{ 11621344268273643014ull, PropId::bordercolor },
    PropEntry{ 11674592879001721745ull, PropId::visiblecheckbtn },
    PropEntry{ 11720374932492612752ull, PropId::wordbreak },
    PropEntry{ 11852225085645501351ull, PropId::itemsize },
    PropEntry{ 11998362212591791088ull, PropId::shadowsize },
    PropEntry{ 12048379283480727430ull, PropId::itemhotimage },
    PropEntry{ 12136687499666440614ull, PropId::fontfile },
    PropEntry{ 12189200217321285997ull, PropId::button2hotimage },
    PropEntry{ 12459044185197775731ull, PropId::mininfo },
    PropEntry{ 12476337319753661815ull, PropId::normalimage },
    PropEntry{ 12502573981298847382ull, PropId::linecolor },
    PropEntry{ 12541152034420510107ull, PropId::FadeButton },
    PropEntry{ 12613261051159514351ull, PropId::step },
    PropEntry{ 12633454823979668292ull, PropId::isstretchfore },
    PropEntry{ 12743703370854514126ull, PropId::itemminwidth },
    PropEntry{ 12771645745017349662ull, PropId::button2disabledimage },
    PropEntry{ 12806531723461904352ull, PropId::itemdisabledbkcolor },
    PropEntry{ 12818901015042040436ull, PropId::count },
    PropEntry{ 12849446073853883495ull, PropId::rich },
    PropEntry{ 13010313787394383978ull, PropId::selitemtextcolor },
    PropEntry{ 13130012740096792991ull, PropId::underline },
    PropEntry{ 13260258112552698837ull, PropId::homepage },
    PropEntry{ 13286255066844632595ull, PropId::itemlinecolor },
    PropEntry{ 13314134955707445469ull, PropId::outradius },
    PropEntry{ 13330009435956128252ull, PropId::texthotcolor },
    PropEntry{ 13442888544795108369ull, PropId::gotoeditbordersize },
    PropEntry{ 13505198832687887368ull, PropId::thumbdisabledimage },
    PropEntry{ 13533817444396550010ull, PropId::roundcorner },
    PropEntry{ 13565327571864837353ull, PropId::thumbnormalimage },
    PropEntry{ 13587051552646665418ull, PropId::hotbkcolor },
    PropEntry{ 13592007848847861961ull, PropId::rhaa },
    PropEntry{ 13639136617617709318ull, PropId::disabledbordercolor },
    PropEntry{ 13672263352284920689ull, PropId::transparent },
    PropEntry{ 13684336228371648309ull, PropId::autocalcwidth },
    PropEntry{ 13795232291291665398ull, PropId::style },
    PropEntry{ 13817014565132192795ull, PropId::visiblefolderbtn },
    PropEntry{ 13828888560709158270ull, PropId::raildisabledimage },
    PropEntry{ 13852217618190508948ull, PropId::keyboard },
    PropEntry{ 13893318284435913207ull, PropId::ischeck },
    PropEntry{ 14025755943645121489ull, PropId::pushedimage },
    PropEntry{ 14044636948056603787ull, PropId::showbutton1 },
    PropEntry{ 14044638047568231998ull, PropId::showbutton2 },
    PropEntry{ 14089346554308210977ull, PropId::hotfont },
    PropEntry{ 14176396743819860870ull, PropId::name },
    PropEntry{ 14192431797075352351ull, PropId::scrollstepsize },
    PropEntry{ 14232688081279808536ull, PropId::itembkcolor },
    PropEntry{ 14238087440465216952ull, PropId::itemhotforeimage },
    PropEntry{ 14308001749955695939ull, PropId::checkboxheight },
    PropEntry{ 14311129532581165304ull, PropId::button1normalimage },
    PropEntry{ 14464081691267888330ull, PropId::multiexpanding },
    PropEntry{ 14482805513236168174ull, PropId::maxheight },
    PropEntry{ 14511455659781637259ull, PropId::button2normalimage },
    PropEntry{ 14534511455063390529ull, PropId::textcolor },
    PropEntry{ 14606691915306934945ull, PropId::bottombordersize },
    PropEntry{ 14786138481385172982ull, PropId::bold },
    PropEntry{ 14829878080644387186ull, PropId::tipvaluecolor },
    PropEntry{ 14859914846945221684ull, PropId::bindtablayoutname },
    PropEntry{ 14900717216237071226ull, PropId::itembkimage },
    PropEntry{ 14910394545690872955ull, PropId::showtext },
    PropEntry{ 14990155402852488052ull, PropId::checkboxwidth },
    PropEntry{ 15201984267101315488ull, PropId::checkitem },
    PropEntry{ 15260173933273191275ull, PropId::checkboxforeimage },
    PropEntry{ 15346076887880800607ull, PropId::linetype },
    PropEntry{ 15505449653002666753ull, PropId::expandable },
    PropEntry{ 15554532729153446875ull, PropId::bordervisible },
    PropEntry{ 15661321448499289690ull, PropId::maxpage },
    PropEntry{ 15666840049116342013ull, PropId::bkcolor },
    PropEntry{ 15756384587757133459ull, PropId::itemdisabledimage },
    PropEntry{ 15829701659144286997ull, PropId::thickness },
    PropEntry{ 15842200671275313599ull, PropId::width },
    PropEntry{ 15852473624173777648ull, PropId::icon },
    PropEntry{ 15877409877253349516ull, PropId::group },
    PropEntry{ 15891757438533361340ull, PropId::ListEx },
    PropEntry{ 15905944839694039366ull, PropId::selectedpushedimage },
    PropEntry{ 15928939931811099664ull, PropId::font },
    PropEntry{ 16040297090211805988ull, PropId::layeredopacity },
    PropEntry{ 16081066351512319407ull, PropId::pageselectedtextcolor },
    PropEntry{ 16235005146863540637ull, PropId::RollText },
    PropEntry{ 16276523228200494474ull, PropId::childvalign },
    PropEntry{ 16299093211167300057ull, PropId::pagehotbkcolor },
    PropEntry{ 16318726642860117084ull, PropId::delaycreate },
    PropEntry{ 16322181097433658740ull, PropId::scaleheader },
    PropEntry{ 16397337321410233055ull, PropId::thumbhotimage },
    PropEntry{ 16398301041066618020ull, PropId::bkcolor1 },
    PropEntry{ 16398303240089874442ull, PropId::bkcolor3 },
    PropEntry{ 16398304339601502653ull, PropId::bkcolor2 },
    PropEntry{ 16426645966754890171ull, PropId::checkboxattr },
    PropEntry{ 16605097406964327489ull, PropId::maxinfo },
    PropEntry{ 16644910942819200306ull, PropId::bkhotimage },
    PropEntry{ 16844237378981328741ull, PropId::gradient },
    PropEntry{ 16921029203541848024ull, PropId::innerstyle },
    PropEntry{ 16972265181084056006ull, PropId::itemshowhtml },
    PropEntry{ 17027832023901477093ull, PropId::statecount },
    PropEntry{ 17051972991580087851ull, PropId::virtualwnd },
    PropEntry{ 17053469695311428740ull, PropId::shadowcolor },
    PropEntry{ 17257192937063639073ull, PropId::visible },
    PropEntry{ 17260177151921719342ull, PropId::vscrollbarstyle },
    PropEntry{ 17280487114681424837ull, PropId::itemattr },
    PropEntry{ 17334114309734800583ull, PropId::disabledfontcolor },
    PropEntry{ 17371695692667471044ull, PropId::shadowcorner },
    PropEntry{ 17402109513065598659ull, PropId::drag },
    PropEntry{ 17411663169601231788ull, PropId::drop },
    PropEntry{ 17538839000766313452ull, PropId::itemcheckimgsize },
    PropEntry{ 17609990078029252610ull, PropId::itemselectedforeimage },
    PropEntry{ 17629315771133053121ull, PropId::iconsize },
    PropEntry{ 17694149802606041935ull, PropId::colorhsl },
    PropEntry{ 17706202657532488179ull, PropId::maxwidth },
    PropEntry{ 17731474190641562772ull, PropId::palletheight },
    PropEntry{ 17823656669960595010ull, PropId::itemaltbk },
    PropEntry{ 17886199807102870849ull, PropId::strikeout },
    PropEntry{ 17909222213897330704ull, PropId::selectedstatecount },
    PropEntry{ 17947175295849381496ull, PropId::pagetextcolor },
    PropEntry{ 17949081542009223621ull, PropId::pagehottextcolor },
    PropEntry{ 17953394579615142639ull, PropId::cursor },
    PropEntry{ 18015793717152399486ull, PropId::text },
    PropEntry{ 18048210491256745158ull, PropId::wanttab },
    PropEntry{ 18158137003206144774ull, PropId::checkboxfocusedimage },
    PropEntry{ 18185779966897477197ull, PropId::railpushedimage },
    PropEntry{ 18285372386425145383ull, PropId::itemalign },
    PropEntry{ 18306888098790575291ull, PropId::stateimage },
    PropEntry{ 18374192456943931703ull, PropId::gotoeditbordercolor },
};
inline constexpr std::size_t kPropCount = sizeof(kPropTable) / sizeof(kPropTable[0]);

constexpr PropId lookup_prop_hash(std::uint64_t h) noexcept
{
    std::size_t lo = 0, hi = kPropCount;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (kPropTable[mid].hash < h)      lo = mid + 1;
        else if (kPropTable[mid].hash > h) hi = mid;
        else return kPropTable[mid].id;
    }
    return PropId::Unknown;
}

constexpr PropId lookup_prop(std::string_view name) noexcept
{
    return lookup_prop_hash(hash_attr(name));
}

// TCHAR pointer entry (LPCTSTR): zero conversion, O(log n)
template <class CharT>
constexpr PropId lookup_prop_t(const CharT* name) noexcept
{
    return lookup_prop_hash(hash_attr_t(name));
}

// ---- Table B: inline DSL subkeys (only valid inside image-like values) ----
enum class SubPropId : std::int32_t { Unknown = -1, padding, gdiplus, rotate, ytiled, mask, hsl, hole, size, align, restype, source, xtiled, res, corner, dest, file, fade };
struct SubPropEntry { std::uint64_t hash; SubPropId id; };

inline constexpr SubPropEntry kSubPropTable[] = {
    SubPropEntry{ 105616063399437942ull, SubPropId::padding },
    SubPropEntry{ 526184092459736669ull, SubPropId::gdiplus },
    SubPropEntry{ 530495622289276298ull, SubPropId::rotate },
    SubPropEntry{ 1309002075746538700ull, SubPropId::ytiled },
    SubPropEntry{ 2249371691748366409ull, SubPropId::mask },
    SubPropEntry{ 3682550422620068672ull, SubPropId::hsl },
    SubPropEntry{ 4623498950138997355ull, SubPropId::hole },
    SubPropEntry{ 5614464919154503228ull, SubPropId::size },
    SubPropEntry{ 5807677297870517278ull, SubPropId::align },
    SubPropEntry{ 6463194505964769969ull, SubPropId::restype },
    SubPropEntry{ 8564681157369146808ull, SubPropId::source },
    SubPropEntry{ 9042670194746662517ull, SubPropId::xtiled },
    SubPropEntry{ 9937664377282143545ull, SubPropId::res },
    SubPropEntry{ 10915924892236784144ull, SubPropId::corner },
    SubPropEntry{ 11955656676351987349ull, SubPropId::dest },
    SubPropEntry{ 12308356992726624803ull, SubPropId::file },
    SubPropEntry{ 17232805691389541363ull, SubPropId::fade },
};
inline constexpr std::size_t kSubPropCount = sizeof(kSubPropTable) / sizeof(kSubPropTable[0]);

constexpr SubPropId lookup_subprop_hash(std::uint64_t h) noexcept
{
    std::size_t lo = 0, hi = kSubPropCount;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (kSubPropTable[mid].hash < h)      lo = mid + 1;
        else if (kSubPropTable[mid].hash > h) hi = mid;
        else return kSubPropTable[mid].id;
    }
    return SubPropId::Unknown;
}

constexpr SubPropId lookup_subprop(std::string_view name) noexcept
{
    return lookup_subprop_hash(hash_attr(name));
}

// TCHAR pointer entry
template <class CharT>
constexpr SubPropId lookup_subprop_t(const CharT* name) noexcept
{
    return lookup_subprop_hash(hash_attr_t(name));
}

} // namespace s3
} // namespace DuiLib

#endif // __S3_ATTR_REGISTRY_H__
