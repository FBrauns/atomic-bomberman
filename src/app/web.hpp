// The browser build (Emscripten). The game keeps its own frame loop; with ASYNCIFY,
// waitFrame() hands control back to the browser once per drawn frame and resumes
// on the next animation frame. The per-user folder (SDL's pref path, /libsdl) is
// kept in the browser's IndexedDB, so settings, schemes and the generated free
// asset set survive a reload.
// Include from one source file only (main.cpp): the EM_JS functions are defined here.
#pragma once

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Mounts /libsdl on IndexedDB and loads what was saved there.
EM_ASYNC_JS(void, ab_web_mount_user_storage, (), {
    try { FS.mkdir('/libsdl'); } catch (e) {}
    FS.mount(IDBFS, {}, '/libsdl');
    await new Promise((resolve) => FS.syncfs(true, (err) => {
        if (err) console.warn('IDBFS load failed', err);
        resolve();
    }));
    // Written back now and then, and when the page is hidden or closed.
    let busy = false;
    const save = () => {
        if (busy) return;
        busy = true;
        FS.syncfs(false, (err) => { busy = false; if (err) console.warn('IDBFS save failed', err); });
    };
    Module.abSaveUserStorage = save;
    setInterval(save, 5000);
    document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') save(); });
    window.addEventListener('pagehide', save);
});

// Ends a drawn frame: the browser shows it and does its own work; the game goes on
// at the next animation frame.
// While the tab is hidden the browser stops animation frames; a timer keeps the game (and a
// hosted network game with it) running. Pages that play sound are not slowed down by Chrome.
EM_ASYNC_JS(void, ab_web_wait_frame, (), {
    await new Promise((resolve) => {
        if (document.hidden) setTimeout(resolve, 16);
        else requestAnimationFrame(() => resolve());
    });
});

EM_JS(void, ab_web_save_user_storage, (), {
    if (Module.abSaveUserStorage) Module.abSaveUserStorage();
});

EM_JS(void, ab_web_quit, (), {
    if (Module.abOnQuit) Module.abOnQuit();
});

// A phone or tablet: the main pointer is a finger.
EM_JS(int, ab_web_touch_screen, (), {
    return (window.matchMedia && window.matchMedia('(pointer: coarse)').matches) ? 1 : 0;
});

namespace ab::web {
inline bool touchScreen() { return ab_web_touch_screen() != 0; }
inline void mountUserStorage() { ab_web_mount_user_storage(); }
inline void waitFrame() { ab_web_wait_frame(); }
inline void saveUserStorage() { ab_web_save_user_storage(); }
inline void quit() { ab_web_quit(); }
}  // namespace ab::web
#endif
