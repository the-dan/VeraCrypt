#if defined(TC_LINUX) || defined(TC_MACOSX)
#include "Main/LanguageStrings.h"
#undef _
#define _(key) LangString[key]
#endif

