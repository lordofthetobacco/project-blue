//! CPU requirements of the native simulation.
//!
//! The sim-only C++ build compiles Jolt with `-mavx2 -mbmi -mf16c -mlzcnt
//! -mpopcnt` (x86-64-v3 class). On an older CPU it would crash with an
//! illegal instruction somewhere inside the first physics step; this check
//! turns that into a clear startup error.

/// The first required instruction-set extension this CPU lacks, if any.
#[must_use]
pub fn missing_native_feature() -> Option<&'static str> {
    #[cfg(target_arch = "x86_64")]
    {
        let features = [
            ("avx2", std::arch::is_x86_feature_detected!("avx2")),
            ("bmi1", std::arch::is_x86_feature_detected!("bmi1")),
            ("f16c", std::arch::is_x86_feature_detected!("f16c")),
            ("lzcnt", std::arch::is_x86_feature_detected!("lzcnt")),
            ("popcnt", std::arch::is_x86_feature_detected!("popcnt")),
        ];
        features
            .into_iter()
            .find(|(_, present)| !present)
            .map(|(name, _)| name)
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        None
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn this_machine_runs_the_native_sim() {
        // The development and CI machines are x86-64-v3 or newer.
        assert_eq!(super::missing_native_feature(), None);
    }
}
