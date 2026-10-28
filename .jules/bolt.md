## 2024-05-18 - [Endianness check optimization]
**Learning:** `LZ4_isLittleEndian` and `XXH_isLittleEndian` functions are called very frequently on modern systems, and the compiler sometimes isn't able to completely optimize them away, because they involve a union and memory check, leading to slight overhead compared to a constant macro when the platform endianness is statically known at compile time via `__BYTE_ORDER__`.
**Action:** Statically define endianness based on `__BYTE_ORDER__` compiler macros if they are available, falling back to the existing runtime check otherwise.
