#pragma once
#include "HistoryTypes.h"
#include "i18n/Localization.h"

namespace shotlet {
enum class HistoryScope { All, Favorites, Today, Yesterday, Week, Type, Tag, Trash };
enum class HistorySort { Newest, Oldest, Name, Largest };
struct HistoryQuery {
    HistoryScope scope = HistoryScope::All;
    HistorySort sort = HistorySort::Newest;
    std::string search, value;
};
int64_t HistoryNow();
int64_t HistoryDayStart(int64_t milliseconds, int offset = 0);
std::string HistoryTime(int64_t milliseconds, const char* format);
std::string HistoryName(const HistoryMetadata& record, Language language = Language::English);
bool HistoryMatches(const HistoryMetadata& record, const HistoryQuery& query, int64_t now);
std::vector<size_t> QueryHistory(const std::vector<HistoryMetadata>& records, const HistoryQuery& query, int64_t now);
} // namespace shotlet
