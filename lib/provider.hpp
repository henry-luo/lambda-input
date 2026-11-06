#pragma once

// publish a complete immutable provider once; equal re-registration is safe.
template<class Operations, class... Fields>
inline bool lambda_provider_register(const Operations** slot, const Operations* operations, Fields... fields) {
    if (!operations || !(... && (operations->*fields))) return false;
    const Operations* expected = nullptr;
    return __atomic_compare_exchange_n(slot, &expected, operations, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ||
           expected == operations;
}

template<class Operations>
inline const Operations* lambda_provider_load(const Operations* const* slot) {
    return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
}
