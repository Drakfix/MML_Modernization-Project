package org.mml.legends;

import org.libsdl.app.SDLActivity;

/* SDL's activity, running libmain.so (our native code) after libSDL3.so */
public class MmlActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
