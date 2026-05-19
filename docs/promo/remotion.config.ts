import { Config } from "@remotion/cli/config";

Config.setVideoImageFormat("jpeg");
Config.setOverwriteOutput(true);
Config.setConcurrency(4);
// Use the system Chrome instead of downloading a separate Headless Shell.
Config.setChromiumOpenGlRenderer("angle");
