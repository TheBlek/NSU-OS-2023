#include <stdio.h>
#include <limits.h>

int main() {
    int result = 0; 
    for (long long i = 0; i < INT_MAX/2; i++) {
        result = (result + i) % 1000000007;
    }
    printf("Oof... Calculated this: %lld\n", result);
}
