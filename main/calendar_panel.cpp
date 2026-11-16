#include "calendar_panel.h"

#include "font_renderer.h"
#include "pjournal_app.h"
#include "ui_helpers.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawFrame(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
}

int calendar_days_in_month(int year, int month) {
    static const int dim[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 30;
    int d = dim[month];
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) d = 29;
    return d;
}

int calendar_day_of_week(int year, int month, int day) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    mktime(&t);
    int w = t.tm_wday;  // 0=Sun
    return w == 0 ? 6 : w - 1;
}

// 严格解析 YYYY-MM-DD。GTD 的 due 是用户可编辑文本，不能用 sscanf("%d")：
// 超长数字会让 %d 溢出（C 标准里是未定义行为），这里用 strtol 再按范围拒绝。
static bool parseDateString(const char *s, int &y, int &m, int &d) {
    char *end = nullptr;
    long py = strtol(s, &end, 10);
    if (end == s || *end != '-') return false;
    char *end2 = nullptr;
    long pm = strtol(end + 1, &end2, 10);
    if (end2 == end + 1 || *end2 != '-') return false;
    char *end3 = nullptr;
    long pd = strtol(end2 + 1, &end3, 10);
    if (end3 == end2 + 1 || *end3 != '\0') return false;
    if (py < 1970 || py > 9999 || pm < 1 || pm > 12 || pd < 1) return false;
    if (pd > calendar_days_in_month((int)py, (int)pm)) return false;
    y = (int)py; m = (int)pm; d = (int)pd;
    return true;
}

void calendar_panel_reset(CalendarPanelState &st, const std::string &dateStr) {
    time_t now_t; time(&now_t); struct tm *tm = localtime(&now_t);
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    int py, pm, pd;
    if (parseDateString(dateStr.c_str(), py, pm, pd)) { y = py; m = pm; d = pd; }
    st.year = y;
    st.month = m;
    st.selDay = d;
}

std::string calendar_panel_date_string(const CalendarPanelState &st) {
    char ds[32];
    snprintf(ds, sizeof(ds), "%04d-%02d-%02d", st.year, st.month, st.selDay);
    return ds;
}

void calendar_panel_draw(const CalendarPanelState &st, const CalendarHasEntryFn &hasEntry) {
    const int boxW = 280;
    const int boxH = 220;
    const int boxX = (SCREEN_W - boxW) / 2;
    const int boxY = (SCREEN_H - boxH) / 2;

    // 不透明白底浮动面板
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);

    // 年月标题与翻月提示
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "%d年%d月", st.year, st.month);
    int hdrW = g_font.textWidth(hdr);
    g_font.drawText(boxX + (boxW - hdrW) / 2, boxY + 12 + g_font.ascent(), hdr, false);
    g_font.drawText(boxX + 8, boxY + 12 + g_font.ascent(), "h", false);
    g_font.drawText(boxX + boxW - 8 - g_font.textWidth("l"),
                    boxY + 12 + g_font.ascent(), "l", false);

    // 星期表头
    const char *wdnames[] = {"一","二","三","四","五","六","日"};
    int colW = boxW / 7;
    int hdrY = boxY + 12 + FONT_H + 4;
    for (int i = 0; i < 7; i++) {
        int cx = boxX + i * colW + (colW - g_font.textWidth(wdnames[i])) / 2;
        g_font.drawText(cx, hdrY + g_font.ascent(), wdnames[i], false);
    }
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, boxX + 4, hdrY + FONT_H + 2, boxW - 8);

    // 日期网格
    int firstDow = calendar_day_of_week(st.year, st.month, 1);
    int dim = calendar_days_in_month(st.year, st.month);
    int gridY = hdrY + FONT_H + 21;

    time_t now_t; time(&now_t); struct tm *now_tm = localtime(&now_t);
    int todayDay = (now_tm->tm_year + 1900 == st.year && now_tm->tm_mon + 1 == st.month)
                   ? now_tm->tm_mday : 0;

    for (int d = 1; d <= dim; d++) {
        int dow = (firstDow + d - 1) % 7;
        int row = (firstDow + d - 1) / 7;
        int cx = boxX + dow * colW;
        int cy = gridY + row * LINE_SPACING;

        bool sel = (d == st.selDay);
        bool today = (d == todayDay);

        char ds[16];
        snprintf(ds, sizeof(ds), "%d", d);
        int tw = g_font.textWidth(ds);
        int dx = cx + (colW - tw) / 2;

        bool has = false;
        if (hasEntry) {
            char date[32];
            snprintf(date, sizeof(date), "%04d-%02d-%02d", st.year, st.month, d);
            has = hasEntry(date);
        }

        TextStyle boldStyle;
        boldStyle.bold = true;

        if (sel) {
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, cx + 2, cy - g_font.ascent(), colW - 4, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 1);
            if (has) g_font.drawTextStyled(dx, cy, ds, boldStyle);
            else g_font.drawText(dx, cy, ds, true);
            u8g2_SetDrawColor(g_u8g2, 0);
        } else {
            if (has) g_font.drawTextStyled(dx, cy, ds, boldStyle);
            else g_font.drawText(dx, cy, ds, false);
            if (today) u8g2_DrawHLine(g_u8g2, dx, cy + 3, tw);
        }
    }
}

CalendarPanelResult calendar_panel_handle_key(int key, CalendarPanelState &st) {
    switch (key) {
    case 0x1B:
        return CalendarPanelResult::Cancel;
    case KEY_LEFT:
        if (st.selDay > 1) st.selDay--;
        break;
    case KEY_RIGHT: {
        int dim = calendar_days_in_month(st.year, st.month);
        if (st.selDay < dim) st.selDay++;
        break;
    }
    case KEY_UP:
    case 'k':
        st.selDay -= 7;
        if (st.selDay < 1) st.selDay = 1;
        break;
    case KEY_DOWN:
    case 'j':
        st.selDay += 7;
        if (st.selDay > calendar_days_in_month(st.year, st.month))
            st.selDay = calendar_days_in_month(st.year, st.month);
        break;
    case 'h':
    case 'H':
        st.month--;
        if (st.month < 1) { st.month = 12; st.year--; }
        if (st.selDay > calendar_days_in_month(st.year, st.month))
            st.selDay = calendar_days_in_month(st.year, st.month);
        break;
    case 'l':
    case 'L':
        st.month++;
        if (st.month > 12) { st.month = 1; st.year++; }
        if (st.selDay > calendar_days_in_month(st.year, st.month))
            st.selDay = calendar_days_in_month(st.year, st.month);
        break;
    case 0x0A:
    case 0x0D:
        return CalendarPanelResult::Accept;
    case 0x7F:
    case 0x08:
        return CalendarPanelResult::Clear;
    default:
        break;
    }
    return CalendarPanelResult::None;
}
