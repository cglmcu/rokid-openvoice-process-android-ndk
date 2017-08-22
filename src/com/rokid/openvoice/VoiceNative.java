package com.rokid.openvoice;

public class VoiceNative{
	
	static{
		System.loadLibrary("openvoice2");
	}
	public static native boolean setup(); 
	
	public static native void startSiren(boolean isopen); 
	
	public static native void setSirenState(int state);
	
	public static native void networkStateChange(boolean isconnected);
	
	public static native void updateStack(String appid);
}
