#pragma once
#include <Windows.h>

namespace wb { HMODULE self_module(); }

namespace trinity
{
    class Mod
    {
    public:
        static Mod& Get() { static Mod m; return m; }
        HMODULE Module() const { return wb::self_module(); }
    };
}
