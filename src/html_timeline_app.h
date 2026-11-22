#pragma once
/** @file html_timeline_app.h
 *  @brief Independent timeline page, server-filtered feed and event navigation.
 */
#include "html_shared.h"
#include "html_timeline_filters.h"
#include "timeline_filter.h"

/** @brief Build the independent timeline page and its server-defined filter editor. */
inline std::string buildHTMLTimelineApp(const std::string &appTitle)
{
    std::string html = R"uTIMELINE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Timeline · __APP_TITLE__</title>
<style>
__SHARED_CSS__
__TIMELINE_FILTER_CSS__
#timelineApp{height:100vh;height:100dvh;display:grid;grid-template-rows:auto 1fr auto;}
a{color:var(--accent2);text-decoration:none;}
a:hover{text-decoration:underline;}
.timeline-header{display:flex;align-items:center;gap:20px;padding:14px 24px;background:var(--panel);border-bottom:1px solid var(--border);}
.timeline-header .brand{font-weight:600;color:var(--accent2);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1;}
.mode-switch{display:flex;gap:4px;padding:4px;background:var(--bg);border:1px solid var(--border);border-radius:9px;}
.mode-switch .btn{border-color:transparent;background:transparent;text-decoration:none;}
.mode-switch .current{background:var(--panel2);color:var(--accent2);}
#timelineMain{overflow:auto;min-height:0;scrollbar-gutter:stable;}
.timeline-content{max-width:1060px;margin:auto;padding:32px 28px;}
.timeline-intro{margin-bottom:28px;}
.timeline-order{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin:16px 0;color:var(--fg-dim);font-size:13px;}
.eyebrow{text-transform:uppercase;letter-spacing:.14em;font-size:11px;color:var(--accent2);font-weight:600;}
h1{font-size:clamp(25px,4vw,36px);font-weight:600;letter-spacing:-.03em;line-height:1.15;margin:10px 0;}
.subtle{color:var(--fg-dim);}
.story{position:relative;margin:0 0 28px 10px;padding:0 0 0 26px;border-left:1px solid var(--border);}
.story:before{content:'';position:absolute;left:-5px;top:7px;width:9px;height:9px;background:var(--accent2);border:2px solid var(--bg);border-radius:50%;box-shadow:0 0 0 3px var(--panel2);}
.story-heading{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px;flex-wrap:wrap;}
.story-heading h2{font-size:18px;font-weight:600;margin:0 0 4px;letter-spacing:-.02em;}
.story-heading .subtle{font-size:12px;}
.story-body{border:1px solid var(--border);border-radius:12px;overflow:hidden;background:var(--panel);box-shadow:0 10px 30px #0002;}
.story-images{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:3px;padding:3px;}
.story-photo{position:relative;display:block;border:0;min-width:0;padding:0;background:var(--panel2);cursor:pointer;overflow:hidden;aspect-ratio:1.55;border-radius:3px;color:var(--fg-dim);}
.story-photo img{width:100%;height:100%;object-fit:cover;display:block;transition:transform .25s;}
.story-photo:hover img{transform:scale(1.04);}
.story-photo:focus-visible,.item:focus-visible{outline:2px solid var(--accent2);outline-offset:-3px;}
.story-photo .ic{position:absolute;inset:40%;}
.story-foot{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:12px 16px;color:var(--fg-dim);font-size:12px;}
.story-foot .btn{font-size:13px;flex-shrink:0;}
.story-foot>.tag-list{flex:1;min-width:0;}
.tag-list{display:flex;flex-wrap:wrap;align-items:center;gap:6px;min-width:0;}
.tag-badge{display:inline-flex;align-items:center;gap:6px;max-width:100%;padding:5px 10px;border-radius:999px;border:1px solid hsl(var(--tag-hue) 48% 66% / .22);background:hsl(var(--tag-hue) 48% 60% / .09);color:hsl(var(--tag-hue) 62% 78%);font-size:11px;font-weight:500;line-height:1.35;overflow-wrap:anywhere;cursor:default;}
.tag-badge:before{content:'';flex:none;width:5px;height:5px;border-radius:50%;background:hsl(var(--tag-hue) 58% 70%);box-shadow:0 0 6px hsl(var(--tag-hue) 60% 65% / .18);}
.tag-badge:hover{background:hsl(var(--tag-hue) 48% 60% / .17);border-color:hsl(var(--tag-hue) 48% 66% / .4);}
.tags-empty{font-size:12px;color:var(--fg-dim);font-style:italic;}
.event-tags{margin-top:18px;padding-top:16px;border-top:1px solid var(--border);}
.event-tags h4{display:flex;align-items:center;gap:8px;margin:0 0 10px;font-size:11px;font-weight:500;color:var(--fg-dim);text-transform:uppercase;letter-spacing:.1em;}
.event-tags .tag-count{font-size:10px;letter-spacing:0;padding:2px 6px;border:1px solid var(--border);border-radius:10px;}
#albumTags{position:absolute;bottom:0;left:0;right:0;z-index:25;padding:40px 60px 24px;background:linear-gradient(transparent,rgba(8,12,19,.88));opacity:0;visibility:hidden;transform:translateY(6px);transition:opacity .18s,transform .18s,visibility .18s;}
#albumTags[hidden]{display:none;}
#albumTags .tag-list{justify-content:center;max-height:28vh;overflow:auto;padding:4px;}
#albumTags .tag-badge{background:hsl(var(--tag-hue) 30% 14% / .92);border-color:hsl(var(--tag-hue) 50% 70% / .32);backdrop-filter:blur(12px);font-size:12px;padding:6px 12px;box-shadow:0 2px 12px #0003;}
#albumLayer.tags-visible #albumTags{opacity:1;visibility:visible;transform:none;}
@media(hover:none){#albumTags{opacity:1;visibility:visible;transform:none;}}
#moreArea{display:flex;justify-content:center;padding:4px 0 36px;}
#btnMore{min-width:165px;justify-content:center;border-radius:20px;}
#btnMore[hidden]{display:none;}
#timelineStatus{padding:8px 20px;background:var(--panel);border-top:1px solid var(--border);color:var(--fg-dim);font-size:12px;}
#pageMessage{margin:0 0 20px;padding:16px;border:1px solid var(--border);border-radius:8px;color:var(--fg-dim);white-space:pre-line;}
#pageMessage:empty{display:none;}
.event-tools{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:24px;}
.event-tools .spacer{flex:1;}
.event-tools [aria-pressed="true"]{background:#23303f;border-color:var(--accent);}
.event-layout{display:grid;grid-template-columns:minmax(0,1fr) 240px;gap:24px;align-items:start;}
#eventDetails{border:1px solid var(--border);border-radius:10px;padding:18px;background:var(--panel);position:sticky;top:20px;overflow-wrap:anywhere;}
#eventDetails h3{margin:0 0 16px;font-size:14px;}
#eventDetails dt{font-size:12px;color:var(--fg-dim);margin-top:12px;}
#eventDetails dd{margin:3px 0 0;font-size:13px;}
.folder-divider{display:flex;align-items:center;gap:12px;margin:24px 0 12px;overflow-wrap:anywhere;}
.folder-divider a{color:var(--fg-dim);font-size:13px;}
.folder-divider:after{content:'';height:1px;background:var(--border);flex:1;min-width:12px;}
.folder-divider:first-child{margin-top:0;}
.event-grid{display:flex;flex-wrap:wrap;gap:12px;margin-bottom:12px;align-content:flex-start;}
.event-grid .item{width:calc(var(--thumb-h) + 24px);min-width:90px;max-width:100%;font:inherit;}
.event-grid .thumb{height:var(--thumb-h);width:100%;background:var(--panel2);overflow:hidden;}
.event-grid .thumb img{width:100%;height:100%;object-fit:contain;}
.event-grid .thumb i{width:32px;height:32px;}
.event-grid .meta{display:none;}
.event-list .event-grid{display:block;}
.event-list .item{width:100%;flex-direction:row;text-align:left;border-bottom:1px solid var(--border);border-radius:0;}
.event-list .thumb{width:var(--thumb-h);flex:none;}
.event-list .name{flex:1;}
.event-list .meta{display:block;white-space:normal;max-width:40%;}
#eventView[hidden],#feedView[hidden]{display:none;}
@media(max-width:760px){
 .timeline-header{padding:10px 12px;gap:8px;flex-wrap:wrap;}
 .timeline-header .brand{min-width:100px;font-size:12px;}
 .mode-switch .btn{padding:5px 8px;}
 .timeline-content{padding:24px 12px;}
 .story{padding-left:16px;margin-left:5px;}
 .story-heading h2{font-size:16px;}
 .story-foot{padding:10px;align-items:flex-start;flex-wrap:wrap;}
 #albumTags{padding:32px 18px 18px;}
 .event-layout{display:block;}
 #eventDetails{position:static;margin-top:24px;}
 .event-tools .spacer{display:none;}
 .story-photo{aspect-ratio:1.1;}
 #albumFooter{font-size:11px;gap:6px;}
 #albumExif{display:none;}
 #albumName{max-width:60%;}
}
@media(prefers-reduced-motion:reduce){.story-photo img,#albumTags{transition:none;}}
</style>
</head>
<body>
<div id="timelineApp">
 <header class="timeline-header">
  <span class="brand">__APP_TITLE__</span>
  <nav class="mode-switch" aria-label="Browse mode">
   <a class="btn" href="?" id="folderMode"><i class="ic ic-folder"></i> Folders</a>
   <a class="btn current" href="?view=timeline" aria-current="page">Timeline</a>
  </nav>
  <button class="btn" id="btnTimelineFilter" title="Filter timeline" aria-label="Timeline filters" aria-haspopup="dialog" aria-controls="timelineFilterDialog" aria-pressed="false"><i class="ic ic-filter"></i><span id="filterBadge" hidden></span></button>
  <button class="btn icon" id="btnRefreshTimeline" title="Refresh timeline" aria-label="Refresh timeline"><i class="ic ic-refresh"></i></button>
 </header>
 <main id="timelineMain">
  <div class="timeline-content">
   <div id="pageMessage" role="status"></div>
   <section id="feedView">
    <div class="timeline-intro"><span class="eyebrow">Your library, in moments</span><h1>Timeline</h1><p class="subtle">A story in every day. Rediscover your pictures and videos, together.</p></div>
    <div class="timeline-order"><label for="timelineOrder">Order</label> <select class="sel" id="timelineOrder"><option value="desc">Newest first (DESC)</option><option value="asc">Oldest first (ASC)</option></select></div>
    <button class="btn" id="filterSummary" hidden></button>
    <div id="stories"></div>
   </section>
   <section id="eventView" hidden>
    <div class="event-tools">
     <a class="btn" href="#"><i class="ic ic-back"></i> Timeline</a>
     <span class="spacer"></span>
     <button class="btn icon" id="eventGrid" title="Grid view" aria-label="Grid view" aria-pressed="true"><i class="ic ic-grid"></i></button>
     <button class="btn icon" id="eventList" title="List view" aria-label="List view" aria-pressed="false"><i class="ic ic-list"></i></button>
     <select class="sel" id="eventSize" title="Thumbnail size" aria-label="Thumbnail size"><option value="72">72 px</option><option value="128" selected>128 px</option><option value="192">192 px</option><option value="256">256 px</option></select>
    </div>
    <div class="timeline-intro"><span class="eyebrow">Event</span><h1 id="eventTitle">Loading event…</h1><p class="subtle" id="eventSubtitle"></p></div>
    <div class="event-layout"><div id="eventItems"></div><aside id="eventDetails"></aside></div>
   </section>
   <div id="moreArea"><button class="btn" id="btnMore">Loading…</button></div>
  </div>
 </main>
 <footer id="timelineStatus" role="status">Loading your moments…</footer>
</div>
__SHARED_ALBUM_HTML__
__TIMELINE_FILTER_HTML__
<div id="toast" role="status"></div>
<script>
'use strict';
__SHARED_DOM_JS__
__SHARED_API_JS__
__SHARED_THUMB_STATE_JS__
__SHARED_ALBUM_JS__
__SHARED_THUMB_JS__
__TIMELINE_FILTER_JS__

var main = $('#timelineMain');
var timelineOrder = new URLSearchParams(location.search).get('order') === 'asc' ? 'asc' : 'desc';
$('#timelineOrder').value = timelineOrder;
var feed = newFeed();
var eventView = null;
var mode = 'feed';
var lastFolder = null;
var lastGrid = null;
var eventSize = 128;
var rotating = false;
var visibleStories = new Set();
function newFeed(){ return {order:timelineOrder, cursor:0, more:true, busy:false, loaded:false, revision:'', scroll:0, stale:false, error:'', status:'', note:''}; }
function active(){ return mode === 'feed' ? feed : eventView; }

async function data(url, options){
  var response = await api(url, options);
  if (!response || !response.ok){
    var error = new Error(response && response.error || 'Could not connect. Please try again.');
    error.stale = !!(response && response.data && response.data.stale);
    throw error;
  }
  return response.data;
}
function message(text){ $('#pageMessage').textContent = text || ''; }
function dayLabel(date){ return date.toLocaleDateString(undefined,{year:'numeric',month:'short',day:'numeric'}); }
function sameDay(a,b){ return a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate(); }
function dateRange(event){
  var start = new Date(event.start * 1000), end = new Date(event.end * 1000);
  if (!sameDay(start,end)) return dayLabel(start) + ' → ' + dayLabel(end);
  var yesterday = new Date(); yesterday.setDate(yesterday.getDate()-1);
  var day = sameDay(start,new Date()) ? 'today' : sameDay(start,yesterday) ? 'yesterday' : dayLabel(start);
  var time = {hour:'2-digit',minute:'2-digit'};
  return day + (event.start === event.end ? '' : ' · ' + start.toLocaleTimeString([],time) + ' → ' + end.toLocaleTimeString([],time));
}
function eventURL(id){ return filteredURL('?act=timeline_event&event=' + enc(id)); }
function updateMore(){
  var view = active(), button = $('#btnMore');
  if (!view) return;
  button.hidden = !view.more && !view.error && !view.stale;
  button.disabled = view.busy;
  button.textContent = view.busy ? 'Loading…' : view.stale ? 'Refresh timeline' : view.error ? 'Retry' : 'More  +';
}
var thumbObserver = new IntersectionObserver(function(entries){
  entries.forEach(function(entry){
    if (!entry.isIntersecting) return;
    var img = entry.target;
    thumbObserver.unobserve(img);
    var key = img.dataset.path + '@' + img.dataset.size;
    if (thumbCache[key]) img.src = thumbCache[key];
    else { img.dataset.thumbKey = key; scheduleThumbs(); }
  });
}, {root:main,rootMargin:'240px'});
function thumbnail(item,size){
  var img = document.createElement('img');
  img.alt = item.name;
  img.dataset.path = item.path;
  img.dataset.size = size;
  img.decoding = 'async';
  thumbObserver.observe(img);
  return img;
}
function clearThumbs(root){
  $$('img',root).forEach(function(img){ thumbObserver.unobserve(img); });
  root.replaceChildren();
}
var storyObserver = new IntersectionObserver(function(entries){
  entries.forEach(function(entry){
    if (entry.isIntersecting) visibleStories.add(entry.target);
    else visibleStories.delete(entry.target);
  });
}, {root:main});

// Shared by story summaries, event details and the current carousel image.
// Use text nodes so digiKam names remain plain text, including markup characters.
function tagBadges(tags, label){
  var list = document.createElement('div'); list.className = 'tag-list';
  list.setAttribute('role','list'); list.setAttribute('aria-label',label);
  (tags || []).forEach(function(tag){
    var badge = document.createElement('span'); badge.className = 'tag-badge';
    badge.setAttribute('role','listitem'); badge.textContent = tag.name;
    badge.title = tag.path || tag.name; badge.setAttribute('aria-label',badge.title);
    var hash = 0;
    for (var i = 0; i < badge.title.length; ++i) hash = (hash * 31 + badge.title.charCodeAt(i)) | 0;
    badge.style.setProperty('--tag-hue', [158,205,260,325,38][(hash >>> 0) % 5]);
    list.appendChild(badge);
  });
  if (!list.childElementCount){
    list.removeAttribute('role');
    var empty = document.createElement('span'); empty.className = 'tags-empty'; empty.textContent = 'No tags'; list.appendChild(empty);
  }
  return list;
}
var albumTags = document.createElement('div'); albumTags.id = 'albumTags'; albumTags.hidden = true;
$('#albumLayer').appendChild(albumTags);
var albumPointer = null, albumTagHideTimer = null;
function updateAlbumTagVisibility(){
  clearTimeout(albumTagHideTimer);
  var media = $('#albumImg.show, #albumVideo.show'), over = false;
  if (albumPointer && media){
    var box = media.getBoundingClientRect();
    over = albumPointer.x >= box.left && albumPointer.x <= box.right && albumPointer.y >= box.top && albumPointer.y <= box.bottom;
  }
  if (over) $('#albumLayer').classList.add('tags-visible');
  // Allow the pointer to cross the space between a small photo and its badges.
  else albumTagHideTimer = setTimeout(function(){ $('#albumLayer').classList.remove('tags-visible'); },350);
}
document.addEventListener('albumitemchange',function(event){
  var item = event.detail;
  albumTags.replaceChildren(); albumTags.hidden = !(item && item.tags && item.tags.length);
  if (!albumTags.hidden){
    albumTags.appendChild(tagBadges(item.tags,'Image tags')); updateAlbumTagVisibility();
  } else {
    clearTimeout(albumTagHideTimer); $('#albumLayer').classList.remove('tags-visible');
  }
});
// Navigation hit zones cover the photo, so measure its bounds instead of :hover.
$('#albumLayer').addEventListener('pointermove',function(event){
  albumPointer = {x:event.clientX,y:event.clientY};
  if (albumTags.contains(event.target)) clearTimeout(albumTagHideTimer);
  else updateAlbumTagVisibility();
});
$('#albumLayer').addEventListener('pointerleave',function(){ albumPointer = null; updateAlbumTagVisibility(); });
$('#albumLayer').addEventListener('keydown',function(event){
  if (event.key === 'Tab'){ clearTimeout(albumTagHideTimer); $('#albumLayer').classList.add('tags-visible'); }
});
$('#albumImg').addEventListener('load',updateAlbumTagVisibility);
$('#albumVideo').addEventListener('loadedmetadata',updateAlbumTagVisibility);
window.addEventListener('resize',updateAlbumTagVisibility);

function carousel(event, index, seed){
  var cache = new Map();
  (seed || []).forEach(function(item){ cache.set(item.index,item); });
  albumProvider = {
    length:event.count,
    get:async function(position){
      if (cache.has(position)) return cache.get(position);
      var page = await data(eventURL(event.id) + '&cursor=' + position + '&limit=1');
      var item = page.items[0];
      if (cache.size >= 120) cache.delete(cache.keys().next().value);
      cache.set(position,item);
      return item;
    }
  };
  openAlbum(index);
}
function storyPhotos(card,event,items){
  var grid = $('.story-images',card);
  clearThumbs(grid);
  items.slice(0,6).forEach(function(item){
    var button = document.createElement('button');
    button.className = 'story-photo';
    button.title = item.path;
    button.setAttribute('aria-label','Open ' + item.name);
    button.appendChild(thumbnail(item,256));
    button.onclick = function(){ carousel(event,item.index,items); };
    grid.appendChild(button);
  });
}
function appendStory(event){
  var card = document.createElement('article');
  card.className = 'story';
  card._event = event;
  card.innerHTML = '<div class="story-heading"><div><h2></h2><span class="subtle"></span></div></div><div class="story-body"><div class="story-images"></div><div class="story-foot"><a class="btn">Open event <span aria-hidden="true">↗</span></a></div></div>';
  $('h2',card).textContent = 'Pictures from ' + dateRange(event);
  $('.story-heading .subtle',card).textContent = event.count.toLocaleString() + ' items';
  $('.story-foot',card).prepend(tagBadges(event.tags,'Event tags'));
  $('a',card).href = '#event=' + enc(event.id);
  $('#stories').appendChild(card);
  storyPhotos(card,event,event.samples);
  storyObserver.observe(card);
}
async function loadFeed(refresh){
  var view = feed;
  if (view.busy || (!view.more && !refresh)) return;
  view.busy = true; view.error = ''; updateMore();
  try {
    var url = refresh ? '?act=timeline_refresh' : '?act=timeline&cursor=' + view.cursor + (view.revision ? '&revision=' + enc(view.revision) : '');
    var page = await data(filteredURL(url + '&order=' + view.order),refresh ? {method:'POST'} : undefined);
    if (view !== feed) return;
    appliedFilter = page.filter; updateFilterIndicator(); updatePrefilterNotice(page);
    view.revision = page.revision; view.cursor = page.next; view.more = page.has_more; view.loaded = true;
    page.events.forEach(appendStory);
    view.status = page.total_events.toLocaleString() + ' events · ' + page.total_items.toLocaleString() + ' items · ' + (view.order === 'asc' ? 'Oldest first' : 'Newest first');
    view.note = !page.total_items ? (filterJSON ? 'No pictures or videos match these filters. Edit the conditions or reset all filters to see your library.' : 'No pictures or videos in this library yet.') : '';
    if (page.error) view.note = page.error;
    if (page.skipped) view.note += (view.note ? '\n' : '') + 'Some database entries were skipped because their files are missing or unavailable.';
    if (mode === 'feed'){ $('#timelineStatus').textContent = view.status; message(view.note); }
  } catch(error){
    if (view !== feed) return;
    view.error = error.message; view.stale = !!error.stale;
    if (mode === 'feed') message(error.message);
  } finally {
    view.busy = false;
    if (view === active()) updateMore();
  }
}
function appendEventItems(items){
  items.forEach(function(item){
    if (item.folder !== lastFolder){
      var divider = document.createElement('div'); divider.className = 'folder-divider';
      var link = document.createElement('a');
      link.href = '?#' + enc(item.folder); link.textContent = 'Folder ' + item.folder;
      divider.appendChild(link); $('#eventItems').appendChild(divider);
      lastGrid = document.createElement('div'); lastGrid.className = 'event-grid';
      $('#eventItems').appendChild(lastGrid); lastFolder = item.folder;
    }
    var button = document.createElement('button'); button.className = 'item'; button.title = item.path;
    var thumb = document.createElement('div'); thumb.className = 'thumb'; thumb.appendChild(thumbnail(item,eventSize * 2));
    var name = document.createElement('div'); name.className = 'name'; name.textContent = item.name;
    var meta = document.createElement('div'); meta.className = 'meta';
    var dateLabel = item.date_source === 'filename' ? 'Filename date' : item.date_source === 'creationDate' ? 'digiKam creation date' : 'digiKam modification date';
    meta.textContent = new Date(item.timestamp * 1000).toLocaleString() + ' · ' + dateLabel;
    button.append(thumb,name,meta);
    button._item = item;
    button.onclick = function(){ carousel(eventView.info,item.index,eventView.items); };
    lastGrid.appendChild(button);
  });
}
function describeEvent(info){
  $('#eventTitle').textContent = 'Pictures from ' + dateRange(info);
  $('#eventSubtitle').textContent = info.count.toLocaleString() + ' items · Grouped by folder';
  var details = $('#eventDetails'); details.replaceChildren();
  var title = document.createElement('h3'); title.textContent = 'Event details'; details.appendChild(title);
  var list = document.createElement('dl');
  [['From',new Date(info.start*1000).toLocaleString()],['To',new Date(info.end*1000).toLocaleString()],['Items',info.count.toLocaleString()]].forEach(function(pair){
    var dt = document.createElement('dt'), dd = document.createElement('dd'); dt.textContent = pair[0]; dd.textContent = pair[1]; list.append(dt,dd);
  });
  details.appendChild(list);
  var tags = document.createElement('section'); tags.className = 'event-tags';
  var heading = document.createElement('h4'); heading.textContent = 'Tags';
  var count = document.createElement('span'); count.className = 'tag-count'; count.textContent = (info.tags || []).length.toLocaleString();
  heading.appendChild(count); tags.append(heading,tagBadges(info.tags,'Event tags')); details.appendChild(tags);
  var help = document.createElement('p'); help.className = 'subtle'; help.textContent = 'Open any picture to browse the whole event. Use ← and → to move between folders.'; details.appendChild(help);
}
async function loadEvent(){
  var view = eventView;
  if (!view || view.busy || !view.more) return;
  view.busy = true; view.error = ''; updateMore();
  try {
    var page = await data(eventURL(view.id) + '&cursor=' + view.cursor);
    if (view !== eventView) return;
    appliedFilter = page.filter; updateFilterIndicator(); updatePrefilterNotice(page);
    view.info = page; view.cursor = page.next; view.more = page.has_more;
    view.items = view.items.concat(page.items);
    appendEventItems(page.items); describeEvent(page);
    $('#timelineStatus').textContent = view.items.length.toLocaleString() + ' of ' + page.count.toLocaleString() + ' items';
  } catch(error){
    if (view !== eventView) return;
    view.error = error.message; view.stale = !!error.stale;
    message(error.message);
  } finally {
    view.busy = false;
    if (view === active()) updateMore();
  }
}
function route(){
  closeAlbum(); albumProvider = null; message('');
  if (mode === 'feed') feed.scroll = main.scrollTop;
  var match = location.hash.match(/^#event=(.+)$/);
  mode = match ? 'event' : 'feed';
  $('#feedView').hidden = mode !== 'feed'; $('#eventView').hidden = mode !== 'event';
  if (mode === 'event'){
    var id;
    try { id = decodeURIComponent(match[1]); } catch(error){ message('Invalid event link'); return; }
    eventView = {id:id,cursor:0,more:true,busy:false,items:[],error:'',stale:false};
    clearThumbs($('#eventItems')); lastFolder = null; lastGrid = null;
    $('#eventTitle').textContent = 'Loading event…'; $('#eventSubtitle').textContent = ''; $('#eventDetails').replaceChildren();
    main.scrollTop = 0; loadEvent();
  } else {
    eventView = null; clearThumbs($('#eventItems'));
    main.scrollTop = feed.scroll;
    if (!feed.loaded) loadFeed();
    else { $('#timelineStatus').textContent = feed.status; message(feed.error || feed.note); }
  }
  updateMore();
}
/** @brief Recreate feed state; filter edits use GET and never invalidate other clients. */
function refreshTimeline(invalidate){
  closeAlbum(); albumProvider = null;
  storyObserver.disconnect(); visibleStories.clear(); clearThumbs($('#stories'));
  thumbCache = {}; albumExifCache = {};
  feed = newFeed();
  mode = 'feed'; eventView = null;
  clearThumbs($('#eventItems'));
  $('#feedView').hidden = false; $('#eventView').hidden = true;
  history.replaceState(null,'',location.pathname + '?view=timeline&order=' + timelineOrder + (filterJSON ? '&filter=' + enc(filterJSON) : ''));
  main.scrollTop = 0; message(''); loadFeed(invalidate !== false);
}
function loadMore(){
  var view = active();
  if (!view || view.busy) return;
  if (view.stale) refreshTimeline();
  else if (mode === 'feed') loadFeed();
  else loadEvent();
}
$('#timelineOrder').onchange = function(){
  timelineOrder = this.value;
  refreshTimeline(false);
};
$('#btnMore').onclick = loadMore;
$('#btnRefreshTimeline').onclick = function(){ refreshTimeline(); };
function setEventList(list){
  $('#eventItems').classList.toggle('event-list',list);
  $('#eventGrid').setAttribute('aria-pressed',String(!list));
  $('#eventList').setAttribute('aria-pressed',String(list));
}
$('#eventGrid').onclick = function(){ setEventList(false); };
$('#eventList').onclick = function(){ setEventList(true); };
$('#eventSize').onchange = function(){
  eventSize = Number(this.value); document.documentElement.style.setProperty('--thumb-h',eventSize+'px');
  $$('.event-grid .item').forEach(function(button){
    clearThumbs($('.thumb',button)); $('.thumb',button).appendChild(thumbnail(button._item,eventSize*2));
  });
};
document.documentElement.style.setProperty('--thumb-h',eventSize+'px');
window.addEventListener('hashchange',route);
new IntersectionObserver(function(entries){
  if (entries[0].isIntersecting){
    var view = active();
    if (view && !view.error && !view.stale) loadMore();
  }
}, {root:main,rootMargin:'300px'}).observe($('#moreArea'));
// Rotate a single visible story per tick, pausing while browsing an event/media.
setInterval(async function(){
  if (document.hidden || mode !== 'feed' || albumOpen() || filterDialog.open || rotating || feed.stale) return;
  var cards = Array.from(visibleStories).filter(function(card){ return card.isConnected && card._event.count > 6; });
  if (!cards.length) return;
  var card = cards[Math.floor(Math.random()*cards.length)], view = feed;
  rotating = true;
  try {
    var page = await data(filteredURL('?act=timeline_samples&event=' + enc(card._event.id)));
    if (view === feed && mode === 'feed' && card.isConnected && !albumOpen()) storyPhotos(card,card._event,page.samples);
  } catch(error){
    if (error.stale && view === feed){ feed.stale = true; message(error.message); updateMore(); }
  } finally { rotating = false; }
},12000);
bindAlbum(); route();
</script>
</body>
</html>)uTIMELINE";
    boost::replace_all(html, "__TIMELINE_FILTER_CSS__", HTMLTimelineFilters::CSS);
    boost::replace_all(html, "__TIMELINE_FILTER_HTML__", HTMLTimelineFilters::HTML);
    boost::replace_all(html, "__TIMELINE_FILTER_JS__", HTMLTimelineFilters::JS);
    boost::replace_all(html, "__TIMELINE_FILTER_FIELDS__", boost::json::serialize(TimelineFilter::fields()));
    return buildHTMLPage(std::move(html), appTitle);
}
