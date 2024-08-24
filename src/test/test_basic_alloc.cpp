#include <radray/types.h>

int main() {
    auto t = radray::new_array<int[]>(16);
    radray::delete_array(t);
    return 0;
}
