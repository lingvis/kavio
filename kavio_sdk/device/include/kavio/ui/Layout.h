#pragma once

/** Lightweight containers for building Widget trees. */

#include <kavio/ui/Widget.h>

namespace kavio::ui {

class KAVIO_UI_API Page : public Widget {
public:
    Page();
};

class KAVIO_UI_API Column : public Widget {
public:
    Column();
};

class KAVIO_UI_API Row : public Widget {
public:
    Row();
};

}  // namespace kavio::ui
