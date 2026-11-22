#pragma once

#include <boost/json.hpp>
#include <string>

namespace PWA {
inline std::string manifest(const std::string &appTitle)
{
    return boost::json::serialize(boost::json::object{
        {"id", "./"},
        {"name", appTitle.empty() ? "Unmanarc's Media Browser" : appTitle},
        {"short_name", appTitle.empty() ? "uMediaBrowser" : appTitle},
        {"description", "Browse your media library by folder or timeline."},
        {"start_url", "./"}, {"scope", "./"}, {"display", "standalone"},
        {"background_color", "#0e1116"}, {"theme_color", "#151a21"},
        {"prefer_related_applications", false},
        {"icons", boost::json::array{
            boost::json::object{{"src", "?act=pwa_icon&size=192"}, {"sizes", "192x192"}, {"type", "image/png"}, {"purpose", "any maskable"}},
            boost::json::object{{"src", "?act=pwa_icon&size=512"}, {"sizes", "512x512"}, {"type", "image/png"}, {"purpose", "any maskable"}}}},
        {"shortcuts", boost::json::array{
            boost::json::object{{"name", "Folders"}, {"url", "./"}},
            boost::json::object{{"name", "Timeline"}, {"url", "?view=timeline"}}}}
    });
}

inline const std::string HEAD = R"pwa(
<link rel="manifest" href="?act=pwa_manifest" crossorigin="use-credentials">
<meta name="theme-color" content="#151a21">
<meta name="mobile-web-app-capable" content="yes">
<style>.btn[hidden]{display:none;}</style>
<script>
(function(){
  var pendingInstall = null;
  function installed(){ return window.matchMedia('(display-mode: standalone)').matches; }
  function update(){
    var button = document.getElementById('btnInstall');
    if (button) button.hidden = !pendingInstall || installed();
  }
  window.addEventListener('beforeinstallprompt', function(event){
    if (!window.isSecureContext || installed()) return;
    event.preventDefault();
    pendingInstall = event;
    update();
  });
  window.addEventListener('appinstalled', function(){ pendingInstall = null; update(); });
  document.addEventListener('DOMContentLoaded', function(){
    var button = document.getElementById('btnInstall');
    if (!button) return;
    button.addEventListener('click', async function(){
      if (!pendingInstall) return;
      var prompt = pendingInstall;
      pendingInstall = null;
      update();
      try { await prompt.prompt(); await prompt.userChoice; }
      catch(error){ console.warn('App installation could not be started', error); }
    });
    update();
  });
})();
</script>
)pwa";

inline const std::string INSTALL_BUTTON = R"pwa(<button class="btn" id="btnInstall" title="Install app" aria-label="Install app" hidden>Install</button>)pwa";
}
