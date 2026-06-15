package com.recreation.launcher

import android.app.NativeActivity
import android.os.Bundle
import android.view.WindowManager
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

/**
 * Hosts the native Vulkan renderer. NativeActivity loads librecreation.so
 * (android.app.lib_name) and hands its ANativeWindow to android_main. We only
 * extend it to run edge-to-edge immersive and keep the screen awake; the engine
 * reads its config from recreation.cfg written by the launcher.
 */
class GameActivity : NativeActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Render over the keyguard and power the screen on, so the surface comes
        // up even when the device is locked (and for on-device verification).
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }
}
