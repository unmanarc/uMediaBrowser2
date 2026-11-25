#pragma once
#include <string>
#include "pwa.h"
#include <boost/algorithm/string/replace.hpp>
#include <Mantids30/Core/Helpers/encoders.h>

// Common styling, thumbnails and media viewer for the independent HTML pages.
namespace HTMLShared {
inline const std::string SHARED_CSS = R"uSHARED(:root{
  --bg:#0e1116; --panel:#151a21; --panel2:#1c232c; --border:#27303b;
  --fg:#e0e6ed; --fg-dim:#8b98a8; --accent:#3b82f6; --accent2:#60a5fa;
  --danger:#ef5350; --gold:#f6c945;
  --item-h:56px;
  --thumb-h:72px;
}
*{box-sizing:border-box;}
html,body{margin:0;height:100%;}
body{
  background:var(--bg); color:var(--fg);
  font:14px/1.4 "Segoe UI", system-ui, -apple-system, Roboto, Helvetica, Arial, sans-serif;
  overflow:hidden;
}
#app{display:grid;height:100vh;grid-template-columns:minmax(0,1fr);grid-template-rows:60px 48px 1fr 40px;}

/* Shared page header keeps the browse modes in the same place in both views. */
.app-header{
  display:flex; align-items:center; gap:8px; padding:0 10px; height:60px;
  background:var(--panel); border-bottom:1px solid var(--border);
}
.app-header .brand{flex:1; min-width:0; font-weight:600; color:var(--accent2); white-space:nowrap; overflow:hidden; text-overflow:ellipsis;}
.header-actions{display:flex; align-items:center; gap:8px; flex:none;}
.mode-switch{display:flex; flex:none; gap:4px; padding:4px; background:var(--bg); border:1px solid var(--border); border-radius:9px;}
.mode-switch .btn{border-color:transparent; background:transparent; text-decoration:none;}
.mode-switch .current{background:var(--panel2); color:var(--accent2);}

/* ----- C: location bar ----- */
#zoneC{
  display:flex; align-items:center; gap:6px; padding:0 10px;
  background:var(--panel2); border-bottom:1px solid var(--border);
}
#pathInput{
  flex:1; min-width:0; background:var(--bg); color:var(--fg);
  border:1px solid var(--border); border-radius:6px; padding:6px 10px; font:inherit;
}

/* ----- F: status bar (moves below the page header in the phone view) ----- */
#zoneF{
  display:flex; justify-content:flex-end; align-items:center; gap:8px;
  padding:0 10px; background:var(--panel); border-top:1px solid var(--border);
}
/* Up/Refresh/Back buttons only exist in the phone header; hidden on desktop. */
#zoneF .mnav{display:none;}
/* The directory-name row is a phone-header element; hidden on desktop.
   (.mbar is the wrapper of the icon bar, so it must stay visible here.) */
#zoneF .mtitle-row{display:none;}

/* ----- B + D + E ----- */
#zoneMid{display:grid; grid-template-columns:260px minmax(0,1fr) 300px; min-height:0;}
#zoneE{
  display:flex; flex-direction:column; min-height:0;
  background:var(--panel); border-left:1px solid var(--border);
  overflow:auto; padding:16px;
}
#detailContent{flex:1 1 auto;}
#selActions{
  flex:none; display:none; justify-content:center; gap:12px;
  padding-top:16px; border-top:1px solid var(--border); margin-top:8px;
}
#selActions.show{display:flex;}
#selActions .btn{min-width:110px; justify-content:center;}
#detailContent h3{margin:0 0 12px; font-size:14px; word-break:break-all;}
#detailContent .d-thumb{display:flex; justify-content:center; margin-bottom:12px;}
#detailContent .d-thumb img{max-width:100%; max-height:260px; border-radius:8px; object-fit:contain; cursor:pointer;}
#detailContent table{width:100%; border-collapse:collapse; font-size:12.5px;}
#detailContent td{padding:5px 0; vertical-align:top;}
#detailContent td.k{color:var(--fg-dim); padding-right:12px; white-space:nowrap; width:38%;}
#detailContent td.v{word-break:break-word;}
#detailContent .exif h2{margin:16px 0 8px; font-size:12px; text-transform:uppercase; letter-spacing:.06em; color:var(--fg-dim);}
#detailContent h2.dirinfo{margin:16px 0 8px; font-size:12px; text-transform:uppercase; letter-spacing:.06em; color:var(--fg-dim);}
#detailContent .empty{color:var(--fg-dim); font-size:13px; text-align:center; padding:40px 0;}
#zoneB{
  background:var(--panel); border-right:1px solid var(--border);
  overflow:auto; padding:6px 0;
}
#zoneD{
  position:relative; overflow:auto; padding:12px;
}
#rubberBand{
  position:absolute; z-index:50; display:none;
  border:1px solid var(--accent); background:rgba(59,130,246,.15);
  border-radius:2px; pointer-events:none;
}

/* Toolbar / icon buttons */
.btn{
  display:inline-flex; align-items:center; gap:6px;
  background:var(--panel2); color:var(--fg); border:1px solid var(--border);
  border-radius:6px; padding:6px 10px; cursor:pointer; font:inherit; white-space:nowrap;
}
.btn:hover{background:#232c38; border-color:#3a4757;}
.btn:disabled{opacity:.4; cursor:not-allowed;}
.btn.icon{width:34px; height:34px; padding:0; justify-content:center;}
.btn.icon>i{width:18px; height:18px; background-repeat:no-repeat; background-position:center; background-size:contain;}
.btn.primary{background:var(--accent); border-color:var(--accent); color:#fff;}
.btn.danger{background:var(--danger); border-color:var(--danger); color:#fff;}
.sel{
  background:var(--panel2); color:var(--fg); border:1px solid var(--border);
  border-radius:6px; padding:5px 8px; font:inherit; cursor:pointer;
}

/* icons (data:image svg) */
i.ic{display:inline-block; width:18px; height:18px; background-repeat:no-repeat; background-position:center; background-size:contain;}
i.ic-back{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M20 11H7.83l5.59-5.59L12 4l-8 8 8 8 1.41-1.41L7.83 13H20v-2z'/%3E%3C/svg%3E");}
i.ic-up{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M12 8l-6 6 1.41 1.41L12 10.83l4.59 4.58L18 14z'/%3E%3C/svg%3E");}
i.ic-forward{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M12 4l-1.41 1.41L16.17 11H4v2h12.17l-5.58 5.59L12 20l8-8z'/%3E%3C/svg%3E");}
i.ic-refresh{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M17.65 6.35A7.95 7.95 0 0 0 12 4a8 8 0 1 0 8 8h-2a6 6 0 1 1-1.76-4.24L13 11h7V4z'/%3E%3C/svg%3E");}
i.ic-fullscreen{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M7 14H5v5h5v-2H7v-3zm-2-4h2V7h3V5H5v5zm12 7h-3v2h5v-5h-2v3zM14 5v2h3v3h2V5h-5z'/%3E%3C/svg%3E");}
i.ic-newfolder{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23f6c945' d='M10 4H4a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-8l-2-2z'/%3E%3Cpath fill='%23e0e6ed' d='M11 12h2v2h2v2h-2v2h-2v-2H9v-2h2z'/%3E%3C/svg%3E");}
i.ic-upload{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M9 16h6v-6h4l-7-7-7 7h4v6zm-4 2h14v2H5v-2z'/%3E%3C/svg%3E");}
i.ic-download{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M19 9h-4V3H9v6H5l7 7 7-7zM5 18v2h14v-2H5z'/%3E%3C/svg%3E");}
i.ic-delete{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23ef5350' d='M6 7h12l-1 14H7L6 7zm4-5h4l1 2H9l1-2zM9 11v7h2v-7H9zm4 0v7h2v-7h-2z'/%3E%3C/svg%3E");}
i.ic-list{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M4 5h2v2H4zm0 4h2v2H4zm0 4h2v2H4zm0 4h2v2H4zM8 5h12v2H8zm0 4h12v2H8zm0 4h12v2H8zm0 4h12v2H8z'/%3E%3C/svg%3E");}
i.ic-grid{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M4 4h7v7H4zm9 0h7v7h-7zM4 13h7v7H4zm9 0h7v7h-7z'/%3E%3C/svg%3E");}
i.ic-close{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M18.3 5.71 12 12l6.3 6.29-1.41 1.41L12 13.41l-4.89 4.89-1.41-1.41L10.59 12 5.7 6.71 7.11 5.3 12 10.59l4.89-4.89z'/%3E%3C/svg%3E");}

/* File type icons */
i.ic-folder{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23f6c945' d='M10 4H4a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-8l-2-2z'/%3E%3C/svg%3E");}
i.ic-file{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%2390a4b7' d='M6 2a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8l-6-6H6zm7 1.5L18 8.5H13V3.5z'/%3E%3C/svg%3E");}
i.ic-image{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%237fb2ff' d='M21 5v14a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2zM8.5 9a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3zM7 14l2-2 2 2 3-4 3 5-4 5-6-6z'/%3E%3C/svg%3E");}
i.ic-album{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%234caf50' d='M4 5a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V5zm3 1.5a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3zM5.5 17.5l3.5-4.5 3 4 2.5-3 4 3.5v1.5h-13v-1.5z'/%3E%3C/svg%3E");}

/* ----- tree (B) ----- */
#zoneB h3{margin:2px 12px; font-size:12px; text-transform:uppercase; letter-spacing:.06em; color:var(--fg-dim);}
.tree-row{
  display:flex; align-items:center; gap:4px; height:28px; cursor:pointer;
  color:var(--fg); border-radius:4px; margin:0 4px; padding-right:4px; user-select:none;
}
.tree-row:hover{background:#202933;}
.tree-row.active{background:#1e3a5f;}
.tree-row .tw{width:16px; text-align:center; color:var(--fg-dim); flex:none;}
.tree-row .lbl{white-space:nowrap; overflow:hidden; text-overflow:ellipsis;}
.tree-row i.ic-folder{width:16px; height:16px; flex:none;}

/* ----- D files ----- */
#filesGrid{display:grid; gap:10px;}
#filesGrid.view-grid{display:flex; flex-wrap:wrap; gap:12px; align-content:flex-start; padding-bottom:24px;}
#filesGrid.view-list{grid-template-columns:1fr;}
.item{
  border:1px solid transparent; border-radius:8px; cursor:pointer;
  display:flex; flex-direction:column; align-items:center; text-align:center;
  padding:10px 8px; gap:6px; background:transparent; color:var(--fg);
}
#filesGrid.view-grid .item{position:relative; width:max-content; max-width:none; height:auto;}
#filesGrid.view-list .item{
  flex-direction:row; text-align:left; padding:6px 10px;
  min-height:var(--item-h); height:auto;
  gap:10px; border-bottom:1px solid var(--border); border-radius:0;
}
.item:hover{background:#161d26;}
.item.selected{background:#1e3a5f; border-color:var(--accent);}
.item.active{border-color:var(--accent2); background:#182433;}
.item.selected.active{background:#1e3a5f; border-color:var(--accent2);}
.item .thumb{display:flex; align-items:center; justify-content:center; border-radius:6px; flex:none;}
.item .thumb img{display:block;}
/* list view thumbnails = square sized by the selected icon size */
#filesGrid.view-list .item .thumb{width:var(--thumb-h); height:var(--thumb-h); overflow:hidden;}
#filesGrid.view-list .item .thumb img{width:100%; height:100%; object-fit:cover;}
#filesGrid.view-list .item .thumb i.ic{width:calc(var(--thumb-h) - 8px); height:calc(var(--thumb-h) - 8px);}
/* icon view thumbnails = fixed height, natural aspect-ratio width */
#filesGrid.view-grid .item .thumb{height:var(--thumb-h); min-width:var(--thumb-h);}
#filesGrid.view-grid .item .thumb img{height:var(--thumb-h); width:auto; max-width:none;}
#filesGrid.view-grid .item .thumb i.ic{width:calc(var(--thumb-h) - 12px); height:calc(var(--thumb-h) - 12px);}
.item .name{
  width:100%; overflow:hidden; text-overflow:ellipsis;
  display:-webkit-box; -webkit-line-clamp:2; -webkit-box-orient:vertical;
  word-break:break-all; font-size:12.5px;
}
#filesGrid.view-list .item .name{font-size:inherit;-webkit-line-clamp:1;}
/* Grid view: one line (the text is already truncated by shortName) so a name
   can never stretch the card past the measured page-wide width. */
#filesGrid.view-grid .item .name{-webkit-line-clamp:1; word-break:normal; overflow-wrap:normal;}
.item .meta{font-size:11px; color:var(--fg-dim); white-space:nowrap;}
#filesGrid.view-list .item .meta{margin-left:auto; display:flex; gap:16px;}
#filesGrid.view-list .item .meta span{min-width:70px;}
#emptyMsg{padding:40px; text-align:center; color:var(--fg-dim);}

/* ----- mobile (phone) view -----
   The status bar (F) becomes a navigation bar with Up / Refresh / Back
   buttons below the shared page header; the file view (D) fills the rest. Location bar (C),
   tree (B) and details (E) are hidden. The ".." entry is no longer rendered
   in the list: navigation is done through the header buttons. ----- */
@media (max-width: 640px), (max-height: 520px) {
  #app{grid-template-rows:60px 66px 1fr;}
  #zoneA{order:-2;}
  .app-header .brand{font-size:12px;}
  .mode-switch .btn{padding:5px 8px;}
  #zoneC, #zoneB, #zoneE{display:none !important;}
  #zoneMid{display:block; min-height:0;}
  /* Keep #zoneD inside the grid track's 1fr height (it has overflow:auto)
     so its vertical scrollbar stays visible on phones. */
  #zoneD{padding:8px; height:100%; min-height:0;}
  /* F moves below the page header and becomes a two-row navigation bar:
     row 1 = the icon buttons (Up/Refresh/Back, view, size, sort),
     row 2 = the current directory name, centered and emphasized.
     (No extra grid children are added: #zoneF itself wraps the two rows
     so #app's explicit 3-row grid stays exactly aligned.) */
  #zoneF{
    order:-1; flex:0 0 66px; height:66px;
    display:flex; flex-direction:column;
    border-top:none; border-bottom:1px solid var(--border);
    gap:0; padding:0; overflow:hidden;
  }
  /* Row 1: the icon bar. It is sized down from the desktop bar (smaller
     buttons, selects and gaps) so that on phones the items shrink to fit the
     viewport width instead of overflowing and hiding icons. JS measures the
     natural (unclamped) width of the row and, when it overflows, scales it
     with a transform so every control stays visible at any phone size. */
  #zoneF .mbar{
    display:flex; align-items:center; flex:0 0 34px; height:34px;
    gap:4px; padding:0 6px; overflow-x:visible; overflow-y:hidden;
  }
  #zoneF .mbar .btn.icon{width:28px; height:28px;}
  #zoneF .mbar .btn.icon>i{width:16px; height:16px;}
  #zoneF .mnav{display:inline-flex;}
  #zoneF .mnav-group{display:inline-flex; align-items:center; gap:4px; flex:none;}
  #zoneF .spacer{flex:1 1 auto; min-width:4px;}
  #zoneF .sel{max-width:88px; flex-shrink:1; min-width:0; padding:4px 6px;}
  /* Row 2: the current directory name, centered and emphasized. */
  #zoneF .mtitle-row{
    display:flex; align-items:center; flex:1 1 auto;
    min-height:0; padding:0 12px; border-top:1px solid var(--border);
  }
  #zoneF .mtitle{
    width:100%; margin:0;
    font-weight:700; font-size:15px; color:var(--fg);
    text-align:center; white-space:nowrap; overflow:hidden;
    text-overflow:ellipsis;
  }
  .item{padding:12px 8px;}
  .item .name{font-size:13px; -webkit-line-clamp:3;}
  #filesGrid.view-grid .item .thumb i.ic{width:calc(var(--thumb-h) - 6px); height:calc(var(--thumb-h) - 6px);}
}

/* context menu */
#ctxMenu{
  position:fixed; z-index:1000; min-width:200px; background:var(--panel2);
  border:1px solid var(--border); border-radius:8px; padding:4px; box-shadow:0 8px 30px rgba(0,0,0,.5);
  display:none;
}
#ctxMenu .mi{
  display:flex; align-items:center; gap:10px; padding:8px 10px; border-radius:5px;
  cursor:pointer; font-size:13px;
}
#ctxMenu .mi:hover{background:#232c38;}
#ctxMenu .mi.disabled{opacity:.4; pointer-events:none;}
#ctxMenu .sep{height:1px; background:var(--border); margin:4px 0;}

/* modal */
#overlay{position:fixed; inset:0; background:rgba(0,0,0,.5); display:none; align-items:center; justify-content:center; z-index:2000;}
#overlay.open{display:flex;}
.dialog{background:var(--panel2); border:1px solid var(--border); border-radius:10px; padding:20px; min-width:320px; box-shadow:0 12px 40px rgba(0,0,0,.6);}
.dialog h3{margin:0 0 14px;}
.dialog input[type=text]{width:100%; background:var(--bg); color:var(--fg); border:1px solid var(--border); border-radius:6px; padding:8px 10px; font:inherit; margin-bottom:12px;}
.dialog .actions{display:flex; justify-content:flex-end; gap:8px;}

/* album (full-screen media layer) */
#albumLayer{
  position:fixed; inset:0; z-index:2500; background:#000;
  display:none; align-items:center; justify-content:center;
}
#albumLayer.open{display:flex;}
#albumMedia{
  display:flex; align-items:center; justify-content:center;
  width:100%; height:100%; padding:64px 16px 16px;
}
#albumImg, #albumVideo{max-width:100%; max-height:100%; object-fit:contain; display:none;}
#albumImg.show, #albumVideo.show{display:block;}
/* real click targets: half of the screen each; the visible buttons are only decorative */
.album-hitzone{position:absolute; top:0; bottom:0; width:50%; z-index:10;}
.album-hitzone.left{left:0;}
.album-hitzone.right{right:0;}
.album-nav{
  position:absolute; top:50%; transform:translateY(-50%);
  z-index:20; pointer-events:none; border:0; background:transparent; cursor:pointer;
  font-size:64px; line-height:1; color:rgba(255,255,255,.35); font-family:Georgia, serif;
  padding:0 22px; user-select:none;
}
.album-hitzone:hover + .album-nav, .album-nav:hover{color:rgba(255,255,255,.85);}
.album-nav.left{left:0;}
.album-nav.right{right:0;}
.album-close{
  position:absolute; top:10px; right:10px; z-index:31;
  background:rgba(255,255,255,.08); border-color:transparent; color:#fff;
}
.album-close:hover{background:rgba(255,255,255,.2);}
#albumFooter{
  position:absolute; left:0; right:0; top:0; z-index:30;
  display:flex; align-items:center; gap:12px; padding:10px 56px 10px 16px;
  background:rgba(20,24,30,.82); backdrop-filter:blur(6px);
  font-size:13px; color:#cfd8e3;
}
#albumName{font-weight:600; color:#fff; word-break:break-all; max-width:28%;}
#albumPos{color:#8b98a8; white-space:nowrap;}
#albumExif{margin-left:auto; text-align:right; color:#8b98a8; font-size:12px; word-break:break-word; max-width:50%;}
#albumExif .chip{display:inline-block; margin-left:10px; white-space:nowrap;}
#albumExif .chip b{color:#cfd8e3; font-weight:600;}

/* toast */
#toast{position:fixed; bottom:18px; left:50%; transform:translateX(-50%); background:var(--panel2); border:1px solid var(--border); border-radius:8px; padding:10px 16px; z-index:3000; display:none; box-shadow:0 6px 20px rgba(0,0,0,.5);}

/* drag & drop upload overlay */
#dropOverlay{
  position:fixed; inset:0; z-index:2900; display:none;
  align-items:center; justify-content:center;
  background:rgba(0,0,0,.55);
}
#dropOverlay.open{display:flex;}
#dropOverlay .dropBox{
  border:3px dashed var(--accent); border-radius:16px;
  background:rgba(21,26,33,.85);
  padding:64px 80px; text-align:center;
  box-shadow:0 0 40px rgba(59,130,246,.35) inset;
}
#dropOverlay .dropBox .ic{width:56px; height:56px; margin:0 auto 16px; display:block;}
#dropOverlay .dropBox .t1{font-size:18px; font-weight:600; margin-bottom:6px;}
#dropOverlay .dropBox .t2{color:var(--fg-dim); font-size:13px;}
)uSHARED";

inline const std::string SHARED_ALBUM_HTML = R"uSHARED(<!-- Album (full-screen media) layer -->
<div id="albumLayer">
  <a id="albumLinkPrev" class="album-hitzone left" href="#" title="Previous"></a>
  <a id="albumLinkNext" class="album-hitzone right" href="#" title="Next"></a>
  <button id="albumBtnPrev" class="album-nav left" title="Previous">&#8249;</button>
  <button id="albumBtnNext" class="album-nav right" title="Next">&#8250;</button>
  <button class="btn icon album-close" id="btnAlbumClose" title="Close album"><i class="ic ic-close"></i></button>
  <div id="albumMedia">
    <img id="albumImg" alt="">
    <video id="albumVideo" controls playsinline preload="metadata"></video>
  </div>
  <header id="albumFooter">
    <span id="albumName"></span>
    <span id="albumPos"></span>
    <span id="albumExif"></span>
  </header>
</div>

)uSHARED";

inline const std::string SHARED_DOM_JS = R"uSHARED(var $ = function(s, r){ return (r||document).querySelector(s); };
var $$ = function(s, r){ return Array.prototype.slice.call((r||document).querySelectorAll(s)); };

)uSHARED";

inline const std::string SHARED_API_JS = R"uSHARED(function getCookie(name){
  var m = document.cookie.match(new RegExp('(?:^|; )' + name + '=([^;]*)'));
  return m ? decodeURIComponent(m[1]) : '';
}
function enc(s){ return encodeURIComponent(s); }

async function api(path, opts){
  opts = opts || {};
  opts.credentials = 'same-origin';
  if (opts.method && opts.method !== 'GET'){
    opts.headers = opts.headers || {};
    opts.headers['X-CSRFToken'] = getCookie('CSRFToken');
  }
  var res = await fetch(path, opts);
  try { return await res.json(); } catch(e){ return {ok:false, error:'Bad response'}; }
}

/** Return the applied effective filter (mandatory AND user), ready for --prefilter. */
window.getCurrentFilter = async function(){
  var current = typeof filterJSON === 'string' ? filterJSON : '';
  var response = await api('?act=current_filter' + (current ? '&filter=' + enc(current) : ''));
  if (!response.ok) throw new Error(response.error || 'Could not export current filter');
  return response.data;
};
window.showCurrentFilter = async function(){
  var filter = await getCurrentFilter();
  console.log(JSON.stringify(filter,null,2));
  return filter;
};
window.downloadCurrentFilter = async function(filename){
  var filter = await getCurrentFilter();
  // Preserve the server-validated compact byte size, including near-limit exports.
  var url = URL.createObjectURL(new Blob([JSON.stringify(filter)],{type:'application/json'}));
  var link = document.createElement('a'); link.href = url; link.download = filename || 'prefilter.json';
  document.body.appendChild(link); link.click(); link.remove();
  setTimeout(function(){ URL.revokeObjectURL(url); },1000);
  return filter;
};

function toast(msg){
  var t = $('#toast');
  t.textContent = msg;
  t.style.display = 'block';
  clearTimeout(t._h);
  t._h = setTimeout(function(){ t.style.display = 'none'; }, 2600);
}

function escapeHtml(s){ return String(s).replace(/[&<>\"]/g, function(c){ return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]; }); }
)uSHARED";

inline const std::string SHARED_THUMB_STATE_JS = R"uSHARED(var thumbCache = {};   // "path@size" -> data: URI
var thumbPending = {}; // "path@size" -> true while a thumb fetch is in-flight
var thumbQueue = [];   // pending "path@size" keys shared by the workers
var thumbQueued = {};  // "path@size" -> true while queued
var thumbWorkersRunning = 0;
var THUMB_CONCURRENCY = 4;  // max simultaneous thumb fetches (one per worker)

)uSHARED";

inline const std::string SHARED_ALBUM_JS = R"uSHARED(// ---------------- ALBUM (full-screen media) ----------------
var albumProvider = null; // Optional {length, get(index)} for paged events.
var albumRenderSerial = 0;
var albumCurrentPath = "";
var albumIdx = -1;      // index into albumList() while the album is open
var albumListCache = [];
var albumExifCache = {}; // full path -> chip html (already rendered)

function isVideo(name){
  return /\.(mp4|webm|ogv|ogg|mov|m4v)$/i.test(name);
}
function isMedia(it){
  return it.type === 'file' && (isImage(it.name) || isVideo(it.name));
}
// Images + videos only (mixed folders: nothing else), respecting filter + sort.
function albumList(){
  if (albumProvider) return albumProvider;
  if (!albumListCache.length){
    albumListCache = sortItems(state.items.filter(isMedia));
  }
  return albumListCache;
}
function albumOpen(){ return $('#albumLayer').classList.contains('open'); }

function openAlbum(preferName){
  var list = albumList();
  if (!list.length){ toast('No images or videos here'); return; }
  albumIdx = 0;
  if (albumProvider){
    albumIdx = Number.isInteger(preferName) ? preferName : 0;
  } else if (preferName){
    var i = list.findIndex(function(it){ return it.name === preferName; });
    if (i >= 0) albumIdx = i;
  } else if (state.selected.length === 1){
    var j = list.findIndex(function(it){ return it.name === state.selected[0]; });
    if (j >= 0) albumIdx = j;
  }
  // Keep the underlying view (including its filters/hash/scroll) on Back.
  if (!albumOpen()) history.pushState(Object.assign({}, history.state, {mediaPreview:true}), '', location.href);
  $('#albumLayer').classList.add('open');
  renderAlbum();
}

function closeAlbum(fromHistory){
  var wasOpen = albumOpen();
  ++albumRenderSerial;
  albumCurrentPath = "";
  document.dispatchEvent(new CustomEvent('albumitemchange', {detail:null}));
  $("#albumImg").removeAttribute("src");
  var v = $('#albumVideo');
  v.pause();
  v.removeAttribute('src');
  v.load();
  $('#albumLayer').classList.remove('open');
  albumIdx = -1;
  // The close button and Escape consume the same entry as Android/browser Back.
  if (wasOpen && fromHistory !== true && history.state && history.state.mediaPreview) history.back();
}

window.addEventListener('popstate', function(){
  if (albumOpen()) closeAlbum(true);
});

function albumNav(dir){
  var list = albumList();
  if (!list.length) return;
  albumIdx = (albumIdx + dir + list.length) % list.length;
  renderAlbum();
}

function albumPath(it){ return it.path || itemPath(it.name); }
function mediaUrl(it){ return '?act=download&file=' + enc(albumPath(it)); }

async function renderAlbum(){
  var serial = ++albumRenderSerial;
  document.dispatchEvent(new CustomEvent('albumitemchange', {detail:null}));
  var list = albumList();
  if (!list.length){ closeAlbum(); return; }
  albumIdx = Math.max(0, Math.min(albumIdx, list.length - 1));
  var it;
  if (list.get){
    $('#albumName').textContent = 'Loading…';
    $('#albumImg').classList.remove('show');
    $('#albumVideo').pause();
    $('#albumVideo').classList.remove('show');
  }
  try { it = list.get ? await list.get(albumIdx) : list[albumIdx]; }
  catch (error){
    if (serial === albumRenderSerial){ closeAlbum(); toast(error.message || 'Could not load media'); }
    return;
  }
  if (serial !== albumRenderSerial || !albumOpen()) return;
  if (!it){ closeAlbum(); return; }
  albumCurrentPath = albumPath(it);
  var img = $('#albumImg'), vid = $('#albumVideo');

  // Stop the previous media.
  if (vid.classList.contains('show')){ vid.pause(); }
  vid.removeAttribute('src'); vid.load();
  img.classList.remove('show');
  vid.classList.remove('show');

  $('#albumName').textContent = it.name;
  $('#albumName').title = albumPath(it);
  $('#albumPos').textContent = (albumIdx + 1) + ' / ' + list.length;
  $('#albumExif').innerHTML = '';

  if (isVideo(it.name)){
    vid.src = mediaUrl(it);
    vid.classList.add('show');
    vid.play().catch(function(){});
    vid.onloadedmetadata = function(){
      var dur = isFinite(vid.duration) ? Math.round(vid.duration) : 0;
      if (dur)
        $('#albumPos').textContent = (albumIdx + 1) + ' / ' + list.length +
          '   \u00b7   ' + Math.floor(dur/60) + ':' + ('0' + dur%60).slice(-2);
    };
  } else {
    img.src = mediaUrl(it);
    img.classList.add('show');
    loadAlbumExif(it);
  }
  document.dispatchEvent(new CustomEvent('albumitemchange', {detail:it}));
  preloadAlbumNext();
}

// Warm the next image so advancing feels instant.
function preloadAlbumNext(){
  var list = albumList();
  if (list.get) return; // Indexed providers fetch only the requested media.
  var nxt = list[(albumIdx + 1) % list.length];
  if (nxt && !isVideo(nxt.name)){
    var p = new Image();
    p.src = mediaUrl(nxt);
  }
}

async function loadAlbumExif(it){
  var full = albumPath(it);
  if (albumExifCache[full] !== undefined){
    $('#albumExif').innerHTML = albumExifCache[full];
    return;
  }
  var html = '';
  try {
    var r = await api('?act=detail&file=' + enc(full));
    if (r && r.ok && r.data){
      var d = r.data, chips = [];
      if (d.width && d.height)
        chips.push('<span class="chip"><b>' + d.width + '\u00d7' + d.height + '</b></span>');
      var e = d.exif || {};
      if (e.Make || e.Model)
        chips.push('<span class="chip"><b>' + escapeHtml([e.Make, e.Model].filter(Boolean).join(' ')) + '</b></span>');
      if (e.LensModel)
        chips.push('<span class="chip">' + escapeHtml(String(e.LensModel)) + '</span>');
      var dt = e.DateTimeOriginal || e.CreateDate;
      if (dt){
        var m = String(dt).match(/^(\d{4}:\d{2}:\d{2})[ T](\d{2}:\d{2}(?::\d{2})?)?/);
        var ds = m ? m[1] : String(dt);
        var ts = m && m[2] ? ' ' + m[2] : '';
        chips.push('<span class="chip">' + escapeHtml(ds + ts) + '</span>');
      }
      if (e.ExposureTime && e.FNumber)
        chips.push('<span class="chip">' + escapeHtml(String(e.ExposureTime)) + 's \u00b7 f/' + escapeHtml(String(e.FNumber)) + '</span>');
      else if (e.ExposureTime)
        chips.push('<span class="chip">' + escapeHtml(String(e.ExposureTime)) + 's</span>');
      else if (e.FNumber)
        chips.push('<span class="chip">f/' + escapeHtml(String(e.FNumber)) + '</span>');
      var iso = e.ISO || e.ISOSpeedRatings;
      if (iso) chips.push('<span class="chip">ISO ' + escapeHtml(String(iso).split('/')[0]) + '</span>');
      if (e.FocalLength)
        chips.push('<span class="chip">' + escapeHtml(String(e.FocalLength).split('/')[0]) + 'mm</span>');
      if (e.GPSLatitude && e.GPSLongitude){
        var lat = parseFloat(e.GPSLatitude) * (String(e.GPSLatitudeRef || '').indexOf('S') >= 0 ? -1 : 1);
        var lon = parseFloat(e.GPSLongitude) * (String(e.GPSLongitudeRef || '').indexOf('W') >= 0 ? -1 : 1);
        chips.push('<span class="chip">' + lat.toFixed(4) + ', ' + lon.toFixed(4) + '</span>');
      }
      html = chips.join('');
    }
  } catch(e){}
  albumExifCache[full] = html;
  if (albumOpen() && albumCurrentPath === full)
    $('#albumExif').innerHTML = html;
}

function bindAlbum(){
  // The visible \u2039 \u203a are decorative: the real links cover each screen half.
  $('#albumLinkPrev').addEventListener('click', function(e){ e.preventDefault(); albumNav(-1); });
  $('#albumLinkNext').addEventListener('click', function(e){ e.preventDefault(); albumNav(1); });
  $('#btnAlbumClose').addEventListener('click', closeAlbum);
  // Click on the empty black area (around the media) closes the album.
  $('#albumMedia').addEventListener('click', function(e){
    if (e.target === $('#albumMedia')) closeAlbum();
  });
}

document.addEventListener('keydown', function(e){
  if (!albumOpen()) return;
  if (['Escape', 'ArrowLeft', 'ArrowRight'].indexOf(e.key) >= 0) e.preventDefault();
  if (e.key === 'Escape') closeAlbum();
  else if (e.key === 'ArrowLeft') albumNav(-1);
  else if (e.key === 'ArrowRight') albumNav(1);
});

function isImage(name){
  return /\.(png|jpe?g|gif|bmp|webp|svg|tiff?|ico|arw|cr2|cr3|nef|orf|dng|pef|raf)$/i.test(name);
}

)uSHARED";

inline const std::string SHARED_THUMB_JS = R"uSHARED(function scheduleThumbs(){
  $$('img[data-thumb-key]').forEach(function(img){
    var key = img.getAttribute('data-thumb-key');
    if (!key || thumbCache[key] || thumbPending[key] || thumbQueued[key]) return;
    thumbQueued[key] = true;
    thumbQueue.push(key);
  });
  while (thumbWorkersRunning < THUMB_CONCURRENCY && thumbQueue.length)
    thumbWorker();
}

async function thumbWorker(){
  thumbWorkersRunning++;
  while (thumbQueue.length){
    var key = thumbQueue.shift();
    delete thumbQueued[key];
    if (thumbCache[key]) continue;
    if (!$$('img[data-thumb-key]').some(function(img){ return img.dataset.thumbKey === key; })) continue;

    var sep = key.lastIndexOf('@');
    var path = key.slice(0, sep);
    var size = key.slice(sep + 1);

    thumbPending[key] = true;
    try {
      var r = await api('?act=thumb&file=' + enc(path) + '&s=' + size);
      if (r && r.ok && r.data && r.data.b64){
        var cachedKeys = Object.keys(thumbCache);
        if (cachedKeys.length >= 240) delete thumbCache[cachedKeys[0]];
        thumbCache[key] = 'data:' + (r.data.mime || 'image/jpeg') + ';base64,' + r.data.b64;
      }
    } catch(e){}
    delete thumbPending[key];

    // Apply to any (possibly re-rendered) <img> waiting on this exact key.
    $$('img[data-thumb-key]').forEach(function(img){
      if (img.getAttribute('data-thumb-key') === key){
        if (thumbCache[key]){
          img.src = thumbCache[key];
          img.removeAttribute('data-thumb-key');
        } else {
          img.parentNode.innerHTML = '<i class="ic ic-file"></i>';
        }
      }
    });
  }
  thumbWorkersRunning--;
  if (thumbQueue.length && thumbWorkersRunning < THUMB_CONCURRENCY)
    thumbWorker();   // queue still has work and we're below the cap: refill
}


)uSHARED";

}
inline std::string buildHTMLPage(std::string html, const std::string &appTitle)
{
    boost::replace_all(html, "__PWA_HEAD__", PWA::HEAD);
    boost::replace_all(html, "__PWA_INSTALL_BUTTON__", PWA::INSTALL_BUTTON);
    boost::replace_all(html, "__SHARED_CSS__", HTMLShared::SHARED_CSS);
    boost::replace_all(html, "__SHARED_ALBUM_HTML__", HTMLShared::SHARED_ALBUM_HTML);
    boost::replace_all(html, "__SHARED_DOM_JS__", HTMLShared::SHARED_DOM_JS);
    boost::replace_all(html, "__SHARED_API_JS__", HTMLShared::SHARED_API_JS);
    boost::replace_all(html, "__SHARED_THUMB_STATE_JS__", HTMLShared::SHARED_THUMB_STATE_JS);
    boost::replace_all(html, "__SHARED_ALBUM_JS__", HTMLShared::SHARED_ALBUM_JS);
    boost::replace_all(html, "__SHARED_THUMB_JS__", HTMLShared::SHARED_THUMB_JS);
    const std::string title = appTitle.empty() ? "Unmanarc's Media Browser" : appTitle;
    boost::replace_all(html, "__APP_TITLE__", Mantids30::Core::Helpers::Encoders::htmlEscape(title));
    return html;
}
