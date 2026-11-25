//! Fixed-capacity UTF-8 strings.

use core::fmt;

use crate::error::Error;

/// A UTF-8 string of at most `N` bytes stored inline (no allocation).
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct BoundedStr<const N: usize> {
    bytes: [u8; N],
    len: u8,
}

impl<const N: usize> BoundedStr<N> {
    /// The empty string.
    pub const EMPTY: Self = Self {
        bytes: [0; N],
        len: 0,
    };

    /// Copies `text`.
    ///
    /// # Errors
    /// [`Error::OutOfRange`] if `text` is longer than `N` bytes (or `N > 255`).
    pub fn new(text: &str) -> Result<Self, Error> {
        Self::from_bytes(text.as_bytes())
    }

    /// Copies `bytes` after validating them as UTF-8.
    ///
    /// # Errors
    /// [`Error::OutOfRange`] if too long, [`Error::InvalidUtf8`] if not UTF-8.
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, Error> {
        if bytes.len() > N {
            return Err(Error::OutOfRange);
        }
        let len = u8::try_from(bytes.len()).map_err(|_| Error::OutOfRange)?;
        core::str::from_utf8(bytes).map_err(|_| Error::InvalidUtf8)?;
        let mut out = Self::EMPTY;
        out.bytes[..bytes.len()].copy_from_slice(bytes);
        out.len = len;
        Ok(out)
    }

    /// The text.
    #[must_use]
    pub fn as_str(&self) -> &str {
        // Only validated UTF-8 is ever stored, so this cannot fail.
        core::str::from_utf8(&self.bytes[..usize::from(self.len)]).unwrap_or("")
    }

    /// Length in bytes.
    #[must_use]
    pub fn len(&self) -> usize {
        usize::from(self.len)
    }

    /// True for the empty string.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

impl<const N: usize> Default for BoundedStr<N> {
    fn default() -> Self {
        Self::EMPTY
    }
}

impl<const N: usize> fmt::Debug for BoundedStr<N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(self.as_str(), f)
    }
}

impl<const N: usize> fmt::Display for BoundedStr<N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}
