#pragma once

class LvglTheme;

// 初始化 LCD 所需主题，并返回当前配置选中的主题。
LvglTheme* InitializeAndGetLcdTheme();

// UI 层从离线资源中创建字体和表情对象。
bool LoadResourceFonts();
bool LoadEmojiResources(bool lock_display = true);
