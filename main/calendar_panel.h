#pragma once

#include <functional>
#include <string>

// 共享月历浮动面板：GTD 截止日期选择与「过往日记」日期筛选共用。
struct CalendarPanelState {
    int year = 2026;
    int month = 1;
    int selDay = 1;
};

enum class CalendarPanelResult { None, Accept, Cancel, Clear };

// 0=周一 .. 6=周日
int calendar_days_in_month(int year, int month);
int calendar_day_of_week(int year, int month, int day);

// dateStr 为 YYYY-MM-DD；空串或非法时回落到今天
void calendar_panel_reset(CalendarPanelState &st, const std::string &dateStr);

std::string calendar_panel_date_string(const CalendarPanelState &st);

// 返回该日期（YYYY-MM-DD）是否有内容，用于粗体标记；传 nullptr 表示不标记
using CalendarHasEntryFn = std::function<bool(const std::string &date)>;

// GTD 风格的 280x220 白底浮动面板，调用前需先绘制底层界面
void calendar_panel_draw(const CalendarPanelState &st, const CalendarHasEntryFn &hasEntry = nullptr);

// 方向键/jk=逐日，h/l=换月，Enter=Accept，Esc=Cancel，退格/Delete=Clear
CalendarPanelResult calendar_panel_handle_key(int key, CalendarPanelState &st);
