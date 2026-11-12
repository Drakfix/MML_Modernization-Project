package org.mml.legends;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/* Starts the app anew (back to the start menu from the game's title screen). It runs in a process
   of its own (the manifest's android:process): it ends the app's process, whose pid it is given,
   starts MmlActivity again, and ends itself. An activity in the foreground may start another, so
   this works where a delayed restart (an alarm) is blocked as a start from the background. */
public class RestartActivity extends Activity {
    static final String EXTRA_PID = "pid";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0)
            Process.killProcess(pid);
        Intent i = new Intent(this, MmlActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
