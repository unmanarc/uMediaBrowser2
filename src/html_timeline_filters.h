#pragma once
#include <string>

/** @file html_timeline_filters.h
 *  @brief Timeline-only full-screen JSON editor; all matching runs on the server.
 * Native dialog isolates focus and makes the background inert. Closing/Escape
 * discards edits; Reset clears applied conditions; Apply serializes the draft.
 * The trusted field catalog comes from TimelineFilter, without querying tags.
 */
/** @brief Embedded assets for the timeline's modal condition editor. */
namespace HTMLTimelineFilters {
/** @brief Responsive styles scoped to timeline filter controls. */
inline const std::string CSS = R"uFILTER(
i.ic-filter{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Cpath fill='%23e0e6ed' d='M3 4h18v2l-7 8v5l-4 2v-7L3 6V4z'/%3E%3C/svg%3E");}
#btnTimelineFilter{position:relative;}
#btnTimelineFilter[aria-pressed="true"]{border-color:var(--accent);color:var(--accent2);background:#3b82f61a;}
#filterBadge{position:absolute;top:-6px;right:-6px;font-size:11px;min-width:18px;padding:1px 5px;border-radius:9px;background:var(--accent);color:white;}
#filterBadge[hidden],#filterSummary[hidden]{display:none;}
#filterSummary{margin:0 0 22px;border-radius:18px;color:var(--accent2);}
#timelineFilterDialog{position:fixed;inset:0;width:100vw;height:100vh;height:100dvh;max-width:none;max-height:none;margin:0;padding:0;border:0;background:var(--bg);color:var(--fg);font:inherit;}
#timelineFilterDialog::backdrop{background:var(--bg);}
#filterForm{height:100%;display:flex;flex-direction:column;}
.filter-header,.filter-footer{flex:none;background:var(--panel);border-bottom:1px solid var(--border);padding:20px 28px;}
.filter-header-inner,.filter-footer-inner{max-width:1060px;margin:auto;display:flex;align-items:center;justify-content:space-between;gap:18px;}
.filter-header h2{font-size:24px;margin:5px 0 0;font-weight:600;letter-spacing:-.03em;}
.filter-body{flex:1;overflow:auto;padding:28px;scrollbar-gutter:stable;}
.filter-container{max-width:1060px;margin:auto;}
.filter-explainer{color:var(--fg-dim);margin:0 0 24px;max-width:760px;line-height:1.7;}
.filter-match{display:flex;align-items:center;flex-wrap:wrap;gap:10px;margin:0 0 24px;}
#timelineFilterDialog select,#timelineFilterDialog input{font:inherit;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:7px;padding:10px 12px;min-width:0;color-scheme:dark;}
#timelineFilterDialog :is(select,input,button):focus-visible{outline:2px solid var(--accent2);outline-offset:3px;}
.filter-rule{padding:20px;margin:0 0 14px;border:1px solid var(--border);border-radius:12px;background:var(--panel);}
.filter-group{padding:16px;margin:0 0 14px;border:1px solid var(--accent);border-radius:12px;min-width:0;}
.filter-group-top{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:16px;}
.filter-group-top .filter-match{margin:0;flex:1;}
.filter-group-actions{display:flex;gap:10px;flex-wrap:wrap;}
.filter-rule-top{display:flex;align-items:center;gap:12px;margin-bottom:18px;}
.filter-rule-number{color:var(--fg-dim);font-size:12px;flex:1;}
.filter-rule-controls{display:grid;grid-template-columns:1.2fr .8fr 1.4fr;gap:16px;}
.filter-rule-controls>div{min-width:0;}
.filter-control{display:flex;flex-direction:column;gap:7px;min-width:0;font-size:12px;color:var(--fg-dim);}
.filter-control :is(input,select){width:100%;font-size:14px;}
.filter-date-zone{margin-top:7px;width:100%;}
.filter-tag-refresh{margin-top:7px;font-size:12px;}
.filter-tag-refresh[hidden]{display:none;}
#filterTagStatus,#filterMetadataStatus{font-size:12px;color:var(--fg-dim);}
#filterTagStatus:empty,#filterMetadataStatus:empty{display:none;}
.filter-remove{color:var(--danger);}
.filter-hint{margin:14px 0 0;font-size:12px;color:var(--fg-dim);line-height:1.6;}
#filterEmpty{padding:46px 20px;text-align:center;border:1px dashed var(--border);border-radius:12px;color:var(--fg-dim);margin-bottom:20px;}
#filterEmpty strong{display:block;color:var(--fg);font-size:17px;font-weight:500;margin-bottom:8px;}
#filterEmpty[hidden]{display:none;}
.filter-add-row{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin-top:20px;}
#filterCount{font-size:12px;color:var(--fg-dim);}
#filterError{color:var(--danger);white-space:pre-line;}
#filterError:empty{display:none;}
.filter-footer{border-top:1px solid var(--border);border-bottom:0;}
.filter-footer-actions{display:flex;gap:10px;}
.filter-apply{background:var(--accent);border-color:var(--accent);color:white;}
@media(max-width:700px){
 .filter-header,.filter-footer{padding:16px;}
 .filter-body{padding:20px 14px;}
 .filter-rule{padding:16px;}
 .filter-rule-controls{grid-template-columns:1fr;gap:12px;}
 .filter-rule-top{gap:8px;flex-wrap:wrap;}
 .filter-group{padding:10px;}
 .filter-group-top .filter-match{flex-basis:100%;}
 .filter-footer-inner{align-items:stretch;flex-direction:column;gap:12px;}
 .filter-footer-actions .btn{flex:1;justify-content:center;}
}
)uFILTER";

/** @brief Accessible modal form with draft cancellation, reset and apply actions. */
inline const std::string HTML = R"uFILTER(
<dialog id="timelineFilterDialog" aria-labelledby="filterTitle" aria-describedby="filterDescription">
 <form id="filterForm">
  <header class="filter-header"><div class="filter-header-inner">
   <div><span class="eyebrow">Refine your library</span><h2 id="filterTitle">Timeline filters</h2></div>
   <button class="btn icon" type="button" id="filterClose" title="Cancel changes" aria-label="Cancel filter changes"><i class="ic ic-close"></i></button>
  </div></header>
  <div class="filter-body"><div class="filter-container">
   <p class="filter-explainer" id="filterDescription">Find moments by tag, creation date or camera metadata. Combine conditions and exclude anything you do not want to see. Filters apply only to the timeline.</p>
   <p class="filter-explainer" id="prefilterNotice" hidden>A mandatory server filter applies to Timeline and Folders. Your conditions narrow those results further (AND). Reset clears only your conditions.</p>
   <p class="filter-match">All main conditions must match (AND). Add a group to match alternatives (OR).</p>
   <p id="filterTagStatus" role="status" aria-live="polite"></p>
   <p id="filterMetadataStatus" role="status" aria-live="polite"></p>
   <div id="filterRules"></div>
   <div id="filterEmpty"><strong>Your whole timeline</strong>Add a condition to narrow your search. Add several conditions for a date range or precise metadata search.</div>
   <div class="filter-add-row"><button class="btn" type="button" id="filterAdd">+ Add condition</button><button class="btn" type="button" id="filterAddGroup">+ Add group (OR / AND)</button><span id="filterCount" role="status"></span></div>
   <p class="filter-hint">Exclude reverses one condition and also includes files where that tag or metadata is missing. Changes take effect when you apply.</p>
   <p id="filterError" role="alert"></p>
  </div></div>
  <footer class="filter-footer"><div class="filter-footer-inner">
   <button class="btn" type="button" id="filterReset">Reset all &amp; show everything</button>
   <div class="filter-footer-actions"><button class="btn" type="button" id="filterCancel">Cancel</button><button class="btn filter-apply" type="submit">Apply &amp; refresh timeline</button></div>
  </div></footer>
 </form>
</dialog>
)uFILTER";

/** @brief Draft editing and JSON transport. No media matching happens in JavaScript.
 * Uses timeline/shared DOM helpers; the URL retains applied JSON on page reloads.
 */
inline const std::string JS = R"uFILTER(
var filterFields = __TIMELINE_FILTER_FIELDS__;
var filterJSON = new URLSearchParams(location.search).get('filter') || '';
var appliedFilter = {rules:[]};
var filterDialog = $('#timelineFilterDialog');
var filterTags = null, filterTagsRequest = null;
var filterValues = null, filterValuesRequest = null;

/** @brief Populate categorical metadata without interpreting database values as markup. */
function populateMetadataSelect(select,selected){
  var values = filterValues === null ? [] : filterValues[select.dataset.field] || [];
  select.replaceChildren(filterOption('',filterValues === null ? 'Loading options…' : values.length ? 'Choose a value…' : 'No values with existing files'));
  values.forEach(function(value){ select.appendChild(filterOption(value,value)); });
  if (selected && !values.includes(selected)){
    var saved = filterOption(selected,selected + (select.dataset.legacy === 'true' ? ' (saved text match)' : ' (unavailable)'));
    saved.disabled = true; select.appendChild(saved);
  }
  select.value = selected || '';
}
/** @brief Refresh database choices on opening the editor and after a gallery rescan. */
function loadFilterValues(){
  if (filterValuesRequest) return filterValuesRequest;
  $('#filterMetadataStatus').textContent = 'Loading camera and lens options…';
  filterValuesRequest = (async function(){
    try {
      var catalog = await data('?act=timeline_filter_values');
      filterValues = catalog.values;
      $$('.filter-metadata-select',$('#filterRules')).forEach(function(select){ populateMetadataSelect(select,select.value); });
      $('#filterMetadataStatus').textContent = '';
    } catch(error){
      $('#filterMetadataStatus').textContent = 'Could not load camera and lens options: ' + error.message + ' Reopen filters or rescan the gallery to retry.';
    } finally { filterValuesRequest = null; }
  })();
  return filterValuesRequest;
}

function updatePrefilterNotice(page){
  $('#prefilterNotice').hidden = !page.prefilter_active;
  $('#filterReset').textContent = page.prefilter_active ? 'Reset user filters' : 'Reset all & show everything';
}

/** @brief Fill native tag lists safely, retaining unavailable applied values as disabled context. */
function populateTagSelect(select,selected){
  select.replaceChildren(filterOption('',filterTags === null ? 'Loading tags…' : filterTags.length ? 'Choose a tag…' : 'No tags with existing files'));
  (filterTags || []).forEach(function(tag){ select.appendChild(filterOption(tag.name,tag.path)); });
  if (selected && !(filterTags || []).some(function(tag){ return tag.name === selected; })){
    var unavailable = filterOption(selected,selected + ' (unavailable)'); unavailable.disabled = true; select.appendChild(unavailable);
  }
  select.value = selected || '';
}
/** @brief Ordinary reads reuse the server snapshot; only the refresh action scans disk. */
function loadFilterTags(rescan){
  if (filterTagsRequest) return filterTagsRequest;
  $('#filterTagStatus').textContent = rescan ? 'Rescanning gallery… This may take a while.' : 'Loading tags…';
  $$('.filter-tag-refresh').forEach(function(button){ button.disabled = true; });
  filterTagsRequest = (async function(){
    try {
      var catalog = await data('?act=' + (rescan ? 'timeline_tags_refresh' : 'timeline_tags'),rescan ? {method:'POST'} : undefined);
      filterTags = catalog.tags;
      $$('.filter-tag-select',$('#filterRules')).forEach(function(select){ populateTagSelect(select,select.value); });
      $('#filterTagStatus').textContent = (rescan ? 'Gallery rescanned. ' : '') + filterTags.length + ' tags with existing files.';
      if (rescan){
        if (filterValuesRequest) await filterValuesRequest;
        await loadFilterValues();
      }
    } catch(error){
      $('#filterTagStatus').textContent = 'Could not load tags: ' + error.message + ' Use the rescan button to retry.';
    } finally {
      filterTagsRequest = null;
      $$('.filter-tag-refresh').forEach(function(button){ button.disabled = false; });
    }
  })();
  return filterTagsRequest;
}

/** @brief Attach identical conditions to feed, event, samples and carousel requests. */
function filteredURL(url){ return url + (filterJSON ? '&filter=' + enc(filterJSON) : ''); }
/** @brief Display applied conditions independently of uncommitted editor changes. */
function updateFilterIndicator(){
  function countConditions(rules){ return rules.reduce(function(n,rule){ return n + (rule.rules ? countConditions(rule.rules) : 1); },0); }
  var count = countConditions(appliedFilter.rules);
  $('#filterBadge').hidden = !count; $('#filterBadge').textContent = count;
  $('#btnTimelineFilter').setAttribute('aria-pressed',String(!!count));
  $('#btnTimelineFilter').setAttribute('aria-label','Timeline filters' + (count ? ', ' + count + ' active' : ''));
  $('#filterSummary').hidden = !count;
  $('#filterSummary').textContent = count + (count === 1 ? ' active condition' : ' active conditions') + ' · Edit filters';
}
/** @brief Create a safe text option, never interpreting user values as markup. */
function filterOption(value,label){
  var option = document.createElement('option'); option.value = value; option.textContent = label; return option;
}
/** @brief Give each rule input a visible, associated accessible label. */
function filterControl(text,control){
  control.setAttribute('aria-label',text);
  var label = document.createElement('label'); label.className = 'filter-control'; label.append(document.createTextNode(text),control); return label;
}
/** @brief Maintain numbering and a shared twenty-item limit, including groups. */
function countFilterRules(){
  var rows = $$('.filter-rule',$('#filterRules'));
  var count = $$('.filter-rule,.filter-group',$('#filterRules')).length;
  rows.forEach(function(row,index){ $('.filter-rule-number',row).textContent = 'Condition ' + (index + 1); });
  $('#filterEmpty').hidden = !!count;
  $('#filterAdd').disabled = $('#filterAddGroup').disabled = count >= 20;
  $$('.filter-group',$('#filterRules')).forEach(function(group){
    group._add.disabled = count >= 20;
    group._addGroup.disabled = count >= 20 || group._depth >= 4;
  });
  $('#filterCount').textContent = count + ' of 20 items (conditions and groups)';
}
/** @brief Compose alternatives or conjunctions inside a clearly bounded group. */
function addFilterGroup(rule,focus,parent,depth){
  if ($$('.filter-rule,.filter-group',$('#filterRules')).length >= 20) return;
  parent = parent || $('#filterRules'); depth = depth || 1;
  if (depth > 4) return;
  rule = rule || {match:'any',rules:[]};
  var group = document.createElement('section'); group.className = 'filter-group'; group._depth = depth;
  var top = document.createElement('div'); top.className = 'filter-group-top';
  var match = document.createElement('select'); match.setAttribute('aria-label','Group match');
  match.append(filterOption('any','Any condition (OR)'),filterOption('all','All conditions (AND)')); match.value = rule.match;
  var label = document.createElement('label'); label.className = 'filter-match'; label.append('Group: ',match);
  var remove = document.createElement('button'); remove.type = 'button'; remove.className = 'btn filter-remove';
  remove.textContent = 'Remove group'; remove.setAttribute('aria-label','Remove group');
  remove.onclick = function(){ group.remove(); countFilterRules(); $('#filterAddGroup').focus(); };
  top.append(label,remove);
  var children = document.createElement('div'); children.className = 'filter-group-rules';
  var actions = document.createElement('div'); actions.className = 'filter-group-actions';
  var add = document.createElement('button'); add.type = 'button'; add.className = 'btn'; add.textContent = '+ Add condition';
  add.onclick = function(){ addFilterRule(null,true,children); };
  var addGroup = document.createElement('button'); addGroup.type = 'button'; addGroup.className = 'btn'; addGroup.textContent = '+ Add group';
  addGroup.onclick = function(){ addFilterGroup(null,true,children,depth + 1); };
  actions.append(add,addGroup); group.append(top,children,actions); parent.appendChild(group);
  group._match = match; group._rules = children; group._add = add; group._addGroup = addGroup;
  rule.rules.forEach(function(child){
    if (child.rules) addFilterGroup(child,false,children,depth + 1);
    else addFilterRule(child,false,children);
  });
  countFilterRules(); if (focus) match.focus();
}
/** @brief Add an editable rule; changing fields resets incompatible values/operators. */
function addFilterRule(rule,focus,parent){
  if ($$('.filter-rule,.filter-group',$('#filterRules')).length >= 20) return;
  rule = rule || {field:'tag',op:'eq',value:'',negate:false};
  var row = document.createElement('section'); row.className = 'filter-rule';
  var top = document.createElement('div'); top.className = 'filter-rule-top';
  var number = document.createElement('span'); number.className = 'filter-rule-number';
  var negate = document.createElement('select'); negate.setAttribute('aria-label','Include or exclude condition');
  negate.append(filterOption('false','Include'),filterOption('true','Exclude (NOT)')); negate.value = String(rule.negate);
  var remove = document.createElement('button'); remove.type = 'button'; remove.className = 'btn filter-remove'; remove.textContent = 'Remove'; remove.setAttribute('aria-label','Remove condition');
  remove.onclick = function(){ row.remove(); countFilterRules(); $('#filterAdd').focus(); };
  top.append(number,negate,remove);
  var controls = document.createElement('div'); controls.className = 'filter-rule-controls';
  var field = document.createElement('select');
  var basic = document.createElement('optgroup'); basic.label = 'Library';
  var metadata = document.createElement('optgroup'); metadata.label = 'digiKam ImageMetadata';
  filterFields.forEach(function(info){ (info.type === 'tag' || info.type === 'date' ? basic : metadata).appendChild(filterOption(info.field,info.label)); });
  field.append(basic,metadata); field.value = rule.field; if (!field.value) field.value = 'tag';
  var op = document.createElement('select'), value = document.createElement('input'); value.required = true;
  var zone = document.createElement('select'); zone.className = 'filter-date-zone'; zone.setAttribute('aria-label','Creation date timezone');
  zone.append(filterOption('','Server local time'),filterOption('Z','UTC'));
  var valueControl = document.createElement('div'); valueControl.append(filterControl('Value',value),zone);
  var refreshTags = document.createElement('button'); refreshTags.type = 'button'; refreshTags.className = 'btn filter-tag-refresh';
  refreshTags.textContent = '↻ Rescan gallery'; refreshTags.setAttribute('aria-label','Rescan gallery for tags');
  refreshTags.title = 'Rebuild the list of existing files. A large gallery may take a while.';
  refreshTags.disabled = !!filterTagsRequest; refreshTags.onclick = function(){ loadFilterTags(true); };
  valueControl.appendChild(refreshTags);
  var hint = document.createElement('p'); hint.className = 'filter-hint';
  controls.append(filterControl('Field',field),filterControl('Comparison',op),valueControl);
  row.append(top,controls,hint); (parent || $('#filterRules')).appendChild(row);
  row._controls = {field:field,op:op,value:value,negate:negate,zone:zone};
  function sync(initial){
    var info = filterFields.find(function(item){ return item.field === field.value; });
    op.replaceChildren();
    if (info.type === 'number') op.append(filterOption('eq','Equals (=)'),filterOption('gt','Greater than (>)'),filterOption('lt','Less than (<)'));
    else if (info.type === 'date') op.append(filterOption('gt','After (>)'),filterOption('lt','Before (<)'));
    else {
      op.append(filterOption('eq','Is exactly'));
      if (initial && info.type === 'text' && rule.op === 'contains') op.append(filterOption('contains','Contains text (saved filter)'));
    }
    if (initial) op.value = rule.op; if (!op.value) op.selectedIndex = 0;
    var nextValue = document.createElement(info.type === 'tag' || info.type === 'text' ? 'select' : 'input'); nextValue.required = true;
    nextValue.setAttribute('aria-label','Value'); value.replaceWith(nextValue); value = nextValue; row._controls.value = value;
    refreshTags.hidden = info.type !== 'tag' && info.type !== 'text';
    refreshTags.setAttribute('aria-label',info.type === 'tag' ? 'Rescan gallery for tags' : 'Rescan gallery for camera and lens options');
    if (info.type === 'tag'){
      value.className = 'filter-tag-select'; populateTagSelect(value,initial ? String(rule.value) : '');
      value.onchange = function(){ value.setCustomValidity(''); };
      zone.hidden = true; hint.textContent = info.hint;
      if (filterTags === null && !filterTagsRequest) loadFilterTags(false);
      return;
    }
    if (info.type === 'text'){
      value.className = 'filter-metadata-select'; value.dataset.field = info.field;
      value.dataset.legacy = String(initial && rule.op === 'contains');
      populateMetadataSelect(value,initial ? String(rule.value) : '');
      value.onchange = function(){
        value.setCustomValidity(''); op.value = 'eq';
        var legacy = op.querySelector('option[value="contains"]'); if (legacy) legacy.remove();
        value.dataset.legacy = 'false';
        populateMetadataSelect(value,value.value); hint.textContent = info.hint;
      };
      zone.hidden = true;
      hint.textContent = initial && rule.op === 'contains' ? 'Saved substring match (ignores A–Z letter case). Choosing a value switches to an exact match.' : info.hint;
      if (filterValues === null && !filterValuesRequest) loadFilterValues();
      return;
    }
    value.type = info.type === 'number' ? 'number' : info.type === 'date' ? 'datetime-local' : 'text';
    value.step = info.type === 'date' ? '1' : 'any'; value.maxLength = 256;
    if (info.type === 'date'){ value.min = '0001-01-01T00:00'; value.max = '9999-12-31T23:59:59'; }
    else { value.removeAttribute('min'); value.removeAttribute('max'); }
    value.placeholder = info.type === 'text' ? 'Text to find…' : 'e.g. 400';
    value.value = initial ? String(rule.value) : ''; value.setCustomValidity('');
    zone.hidden = info.type !== 'date'; zone.value = '';
    if (initial && info.type === 'date'){
      var date = String(rule.value).replace(' ','T'), suffix = date.match(/(Z|[+-]\d{2}:\d{2})$/);
      if (suffix){
        if (suffix[1] !== 'Z') zone.appendChild(filterOption(suffix[1],'UTC' + suffix[1]));
        zone.value = suffix[1]; date = date.slice(0,-suffix[1].length);
      }
      value.value = date.length === 10 ? date + 'T00:00:00' : date;
    }
    hint.textContent = info.hint;
    value.oninput = function(){ value.setCustomValidity(''); };
  }
  field.onchange = function(){ sync(false); value.focus(); };
  sync(true); countFilterRules(); if (focus) value.focus();
}
/** @brief Open a disposable draft; native Escape and Cancel never change the feed. */
function openTimelineFilters(){
  closeAlbum(); $('#filterRules').replaceChildren(); $('#filterError').textContent = '';
  appliedFilter.rules.forEach(function(rule){
    if (rule.rules) addFilterGroup(rule,false); else addFilterRule(rule,false);
  });
  countFilterRules(); filterDialog.showModal(); $('#filterAdd').focus();
  loadFilterTags(false);
  loadFilterValues();
}
/** @brief Commit JSON and regenerate the timeline without invalidating other clients. */
function applyTimelineFilter(filter){
  appliedFilter = filter; filterJSON = filter.rules.length ? JSON.stringify(filter) : '';
  updateFilterIndicator(); filterDialog.close(); refreshTimeline(false);
}
$('#btnTimelineFilter').onclick = openTimelineFilters;
$('#filterSummary').onclick = openTimelineFilters;
$('#filterAdd').onclick = function(){ addFilterRule(null,true); };
$('#filterAddGroup').onclick = function(){ addFilterGroup(null,true); };
$('#filterClose').onclick = $('#filterCancel').onclick = function(){ filterDialog.close(); };
$('#filterReset').onclick = function(){ applyTimelineFilter({rules:[]}); };
$('#filterForm').onsubmit = function(event){
  event.preventDefault(); var invalid = false;
  $('#filterError').textContent = '';
  function readRules(parent){ return Array.from(parent.children).map(function(row){
    if (row.classList.contains('filter-group')){
      var children = readRules(row._rules);
      if (!children.length){ $('#filterError').textContent = 'Add a condition to each group or remove the empty group.'; invalid = true; }
      return {match:row._match.value,rules:children};
    }
    var c = row._controls, info = filterFields.find(function(item){ return item.field === c.field.value; });
    var value = info.type === 'number' ? Number(c.value.value) : info.type === 'tag' || info.type === 'text' ? c.value.value : c.value.value.trim();
    if (info.type === 'date') value += (value.length === 16 ? ':00' : '') + c.zone.value;
    if (typeof value === 'string' && (!value || new TextEncoder().encode(value).length > 256)){
      c.value.setCustomValidity('Enter 1–256 bytes of text.'); c.value.reportValidity(); invalid = true;
    }
    if (info.type === 'number' && !Number.isFinite(value)) invalid = true;
    return {field:c.field.value,op:c.op.value,value:value,negate:c.negate.value === 'true'};
  }); }
  var rules = readRules($('#filterRules'));
  if (invalid) return;
  var filter = {rules:rules};
  if (new TextEncoder().encode(JSON.stringify(filter)).length > 8192){ $('#filterError').textContent = 'These conditions are too long. Shorten a value or remove a condition.'; return; }
  applyTimelineFilter(filter);
};
)uFILTER";
}
