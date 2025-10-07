package com.ysoftware;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

public class MainActivity extends Activity {
    static { 
        System.loadLibrary("sdl3_android");
        System.loadLibrary("main");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        startActivity(new Intent(this, org.libsdl.app.SDLActivity.class));
        finish();
    }
}
