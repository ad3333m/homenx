#include "XmbLayout.hpp"
#include <cassert>

int main() {
    using namespace cards::xmb;
    // Empty and end-of-library input must never produce an invalid index.
    assert(step(0, -1, 0) == 0);
    assert(step(0, 1, 0) == 0);
    assert(step(0, -1, 1) == 0);
    assert(step(0, 1, 1) == 0);
    assert(step(0, -1, 1000) == 0);
    assert(step(999, 1, 1000) == 999);
    assert(step(4, -1, 10) == 3);
    assert(step(4, 1, 10) == 5);
    // Selection stays anchored as the list scrolls, including large libraries.
    for (int selected : {0, 1, 50, 999}) {
        auto item = itemRect(selected, static_cast<float>(selected));
        assert(item.center().x == axisX);
        assert(item.center().y == itemY);
        assert(item.width == 76.f);
        auto hit = itemHit(selected, static_cast<float>(selected));
        assert(hit.contains(500.f, itemY));
        assert(!hit.contains(500.f, itemY + itemPitch));
    }
    auto halfway = itemRect(5, 4.5f);
    assert(halfway.center().y == itemY + itemPitch * .5f);
    assert(halfway.width == 60.f);
    assert(categoryX(2, 2) == axisX);
    assert(categoryX(1, 2) == axisX - categoryPitch);
    assert(categoryX(3, 2) == axisX + categoryPitch);
}
