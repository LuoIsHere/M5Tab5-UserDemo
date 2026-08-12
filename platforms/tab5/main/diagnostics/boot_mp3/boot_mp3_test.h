#pragma once

/**
 * Run the isolated boot-time MP3 playback diagnostic.
 *
 * This function does not return. After one playback it stays in a low-activity
 * delay loop, or recreates the player and repeats when configured by menuconfig.
 */
void tab5_boot_mp3_test_run();
