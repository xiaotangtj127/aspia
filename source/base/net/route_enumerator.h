//
// Aspia Project
// Copyright (C) 2016-2025 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#ifndef BASE_NET_ROUTE_ENUMERATOR_H
#define BASE_NET_ROUTE_ENUMERATOR_H

#include "base/macros_magic.h"

#include <cstdint>
#include <memory>
#include <string>

namespace base {

class RouteEnumerator
{
public:
    RouteEnumerator();
    ~RouteEnumerator();

    bool isAtEnd() const;
    void advance();

    std::string destonation() const;
    std::string mask() const;
    std::string gateway() const;
    uint32_t metric() const;

private:
    std::unique_ptr<uint8_t[]> forward_table_buffer_;
    uint32_t num_entries_ = 0;
    uint32_t pos_ = 0;

    DISALLOW_COPY_AND_ASSIGN(RouteEnumerator);
};

} // namespace base

#endif // BASE_NET_ROUTE_ENUMERATOR_H
