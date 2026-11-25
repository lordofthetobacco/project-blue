//! Bit packing: writer, reader, size measurement and the symmetric
//! [`Stream`] trait every codec in the crate is written against.
//!
//! Bit order is little-endian at both levels: stream bit `i` is bit `i % 8`
//! (value `1 << (i % 8)`) of byte `i / 8`, and an `n`-bit value is written
//! least significant bit first. Writing `0b101` in 3 bits and then `0b1` in
//! 1 bit therefore produces the byte `0b0000_1101`.

use crate::error::Error;
use crate::text::BoundedStr;

/// Number of bits needed to store every integer in `0..=range`.
#[must_use]
pub const fn bits_required(range: u32) -> u32 {
    32 - range.leading_zeros()
}

/// Number of bytes `bits` occupy once padded to a byte boundary.
#[must_use]
pub const fn bytes_for_bits(bits: usize) -> usize {
    bits.div_ceil(8)
}

/// Symmetric serialisation: the same `serialize` function writes when the
/// stream is a [`BitWriter`] or [`Measure`] and reads when it is a
/// [`BitReader`]. Values are passed by `&mut` so a reader can fill them.
pub trait Stream {
    /// True for streams that produce bits ([`BitWriter`], [`Measure`]).
    const WRITING: bool;

    /// Moves the low `bits` bits of `value` (`bits <= 32`). Writers fail with
    /// [`Error::OutOfRange`] when `value` has bits above `bits` set.
    fn serialize_bits(&mut self, value: &mut u32, bits: u32) -> Result<(), Error>;

    /// Bits moved so far.
    fn bit_pos(&self) -> usize;

    /// One bit: 1 = true.
    fn bool(&mut self, value: &mut bool) -> Result<(), Error> {
        let mut bit = u32::from(*value);
        self.serialize_bits(&mut bit, 1)?;
        *value = bit != 0;
        Ok(())
    }

    /// An unsigned integer in `min..=max`, sent as `value - min` in
    /// `bits_required(max - min)` bits. Requires `min <= max`.
    fn bounded_u32(&mut self, value: &mut u32, min: u32, max: u32) -> Result<(), Error> {
        let range = max.checked_sub(min).ok_or(Error::Usage)?;
        let mut raw = 0;
        if Self::WRITING {
            if *value < min || *value > max {
                return Err(Error::OutOfRange);
            }
            raw = *value - min;
        }
        self.serialize_bits(&mut raw, bits_required(range))?;
        if !Self::WRITING {
            if raw > range {
                return Err(Error::OutOfRange);
            }
            *value = min + raw;
        }
        Ok(())
    }

    /// A signed integer in `min..=max`, sent as `value - min` in
    /// `bits_required(max - min)` bits. Requires `min <= max`.
    fn bounded_i32(&mut self, value: &mut i32, min: i32, max: i32) -> Result<(), Error> {
        let range = u32::try_from(i64::from(max) - i64::from(min)).map_err(|_| Error::Usage)?;
        let mut raw = 0;
        if Self::WRITING {
            if *value < min || *value > max {
                return Err(Error::OutOfRange);
            }
            raw = u32::try_from(i64::from(*value) - i64::from(min)).map_err(|_| Error::Usage)?;
        }
        self.serialize_bits(&mut raw, bits_required(range))?;
        if !Self::WRITING {
            if raw > range {
                return Err(Error::OutOfRange);
            }
            *value =
                i32::try_from(i64::from(min) + i64::from(raw)).map_err(|_| Error::OutOfRange)?;
        }
        Ok(())
    }

    /// An unsigned integer in `0..=max` stored in a `u8`.
    fn bounded_u8(&mut self, value: &mut u8, max: u8) -> Result<(), Error> {
        let mut wide = u32::from(*value);
        self.bounded_u32(&mut wide, 0, u32::from(max))?;
        *value = u8::try_from(wide).map_err(|_| Error::OutOfRange)?;
        Ok(())
    }

    /// A signed integer in `min..=max` stored in an `i8`.
    fn bounded_i8(&mut self, value: &mut i8, min: i8, max: i8) -> Result<(), Error> {
        let mut wide = i32::from(*value);
        self.bounded_i32(&mut wide, i32::from(min), i32::from(max))?;
        *value = i8::try_from(wide).map_err(|_| Error::OutOfRange)?;
        Ok(())
    }

    /// 8 bits.
    fn u8(&mut self, value: &mut u8) -> Result<(), Error> {
        self.bounded_u8(value, u8::MAX)
    }

    /// 16 bits.
    fn u16(&mut self, value: &mut u16) -> Result<(), Error> {
        let mut wide = u32::from(*value);
        self.serialize_bits(&mut wide, 16)?;
        *value = u16::try_from(wide).map_err(|_| Error::OutOfRange)?;
        Ok(())
    }

    /// 16 bits, two's complement.
    fn i16(&mut self, value: &mut i16) -> Result<(), Error> {
        let mut raw = value.cast_unsigned();
        self.u16(&mut raw)?;
        *value = raw.cast_signed();
        Ok(())
    }

    /// 32 bits.
    fn u32(&mut self, value: &mut u32) -> Result<(), Error> {
        self.serialize_bits(value, 32)
    }

    /// 64 bits: the low 32 bits, then the high 32 bits.
    fn u64(&mut self, value: &mut u64) -> Result<(), Error> {
        let mut low = (*value & 0xFFFF_FFFF) as u32;
        let mut high = (*value >> 32) as u32;
        self.serialize_bits(&mut low, 32)?;
        self.serialize_bits(&mut high, 32)?;
        *value = u64::from(low) | (u64::from(high) << 32);
        Ok(())
    }

    /// Variable-length unsigned integer: groups of 6 bits, least significant
    /// group first, each followed by a continuation bit (1 = another group
    /// follows). At most 6 groups; the 6th may only use its low 2 bits and
    /// must end the value. The encoding is canonical: a group of 0 can only
    /// be the first group.
    fn varuint(&mut self, value: &mut u32) -> Result<(), Error> {
        if Self::WRITING {
            let mut rest = *value;
            loop {
                let mut group = rest & 0x3F;
                rest >>= 6;
                let mut more = rest != 0;
                self.serialize_bits(&mut group, 6)?;
                self.bool(&mut more)?;
                if !more {
                    return Ok(());
                }
            }
        }
        let mut result: u32 = 0;
        for index in 0..6 {
            let mut group = 0;
            let mut more = false;
            self.serialize_bits(&mut group, 6)?;
            self.bool(&mut more)?;
            if index == 5 && (group > 0x3 || more) {
                return Err(Error::OutOfRange);
            }
            result |= group << (6 * index);
            if !more {
                if index > 0 && group == 0 {
                    return Err(Error::NonCanonical);
                }
                *value = result;
                return Ok(());
            }
        }
        Err(Error::OutOfRange)
    }

    /// Variable-length signed integer: zig-zag mapped
    /// (`0, -1, 1, -2, ...` -> `0, 1, 2, 3, ...`), then [`Stream::varuint`].
    fn varint(&mut self, value: &mut i32) -> Result<(), Error> {
        let mut raw = zigzag_encode(*value);
        self.varuint(&mut raw)?;
        *value = zigzag_decode(raw);
        Ok(())
    }

    /// An IEEE-754 binary32 as its 32-bit pattern. Must be finite.
    fn f32(&mut self, value: &mut f32) -> Result<(), Error> {
        if Self::WRITING && !value.is_finite() {
            return Err(Error::OutOfRange);
        }
        let mut raw = value.to_bits();
        self.serialize_bits(&mut raw, 32)?;
        let decoded = f32::from_bits(raw);
        if !decoded.is_finite() {
            return Err(Error::OutOfRange);
        }
        *value = decoded;
        Ok(())
    }

    /// Three [`Stream::f32`] values, x then y then z.
    fn vec3(&mut self, value: &mut [f32; 3]) -> Result<(), Error> {
        for component in value.iter_mut() {
            self.f32(component)?;
        }
        Ok(())
    }

    /// A UTF-8 string of at most `N` bytes: the byte length as
    /// `bounded(0, N)`, then the bytes, 8 bits each.
    fn string<const N: usize>(&mut self, value: &mut BoundedStr<N>) -> Result<(), Error> {
        let max = u32::try_from(N).map_err(|_| Error::Usage)?;
        let mut len = u32::try_from(value.len()).map_err(|_| Error::OutOfRange)?;
        self.bounded_u32(&mut len, 0, max)?;
        let len = len as usize;
        let mut bytes = [0u8; N];
        if Self::WRITING {
            bytes[..len].copy_from_slice(value.as_str().as_bytes());
        }
        for byte in &mut bytes[..len] {
            self.u8(byte)?;
        }
        if !Self::WRITING {
            *value = BoundedStr::from_bytes(&bytes[..len])?;
        }
        Ok(())
    }
}

/// Zig-zag maps a signed integer onto an unsigned one.
#[must_use]
pub const fn zigzag_encode(value: i32) -> u32 {
    (value.cast_unsigned() << 1) ^ (value >> 31).cast_unsigned()
}

/// Inverse of [`zigzag_encode`].
#[must_use]
pub const fn zigzag_decode(value: u32) -> i32 {
    (value >> 1).cast_signed() ^ (-((value & 1).cast_signed()))
}

/// Writes bits into a caller-provided buffer. Never allocates.
///
/// Bytes are fully defined up to the current position: bits beyond it in the
/// last partial byte are cleared when [`BitWriter::finish`] pads.
#[derive(Debug)]
pub struct BitWriter<'a> {
    buf: &'a mut [u8],
    pos: usize,
    limit: usize,
}

impl<'a> BitWriter<'a> {
    /// A writer over the whole of `buf`.
    #[must_use]
    pub fn new(buf: &'a mut [u8]) -> Self {
        let limit = buf.len() * 8;
        Self { buf, pos: 0, limit }
    }

    /// Current position in bits.
    #[must_use]
    pub fn pos(&self) -> usize {
        self.pos
    }

    /// The bit position writes may not pass.
    #[must_use]
    pub fn limit(&self) -> usize {
        self.limit
    }

    /// Moves the write limit, clamped to the buffer size.
    pub fn set_limit(&mut self, limit_bits: usize) {
        self.limit = limit_bits.min(self.buf.len() * 8);
    }

    /// Bits left before the limit.
    #[must_use]
    pub fn bits_left(&self) -> usize {
        self.limit.saturating_sub(self.pos)
    }

    /// Moves the position back to `pos` (for example after an append that
    /// did not fit). Positions ahead of the current one are ignored.
    pub fn rewind(&mut self, pos: usize) {
        self.pos = self.pos.min(pos);
    }

    /// Writes the low `bits` bits of `value`, LSB first.
    ///
    /// # Errors
    /// [`Error::OutOfRange`] if `value` does not fit in `bits` or
    /// `bits > 32`; [`Error::Overflow`] if the limit would be passed. Nothing
    /// is written on error.
    pub fn write_bits(&mut self, value: u32, bits: u32) -> Result<(), Error> {
        if bits > 32 || (bits < 32 && (value >> bits) != 0) {
            return Err(Error::OutOfRange);
        }
        if self.pos + bits as usize > self.limit {
            return Err(Error::Overflow);
        }
        let mut rest = u64::from(value);
        let mut remaining = bits as usize;
        while remaining > 0 {
            let index = self.pos / 8;
            let offset = self.pos % 8;
            let take = (8 - offset).min(remaining);
            let mask = ((1u16 << take) - 1) as u8;
            let chunk = (rest as u8) & mask;
            let Some(byte) = self.buf.get_mut(index) else {
                return Err(Error::Overflow);
            };
            *byte = (*byte & !(mask << offset)) | (chunk << offset);
            rest >>= take;
            remaining -= take;
            self.pos += take;
        }
        Ok(())
    }

    /// Pads with zero bits to the next byte boundary (ignoring the limit,
    /// never the buffer) and returns the number of bytes written.
    ///
    /// # Errors
    /// [`Error::Overflow`] if the buffer has no room for the padding.
    pub fn finish(mut self) -> Result<usize, Error> {
        let padding = (8 - self.pos % 8) % 8;
        self.limit = self.buf.len() * 8;
        self.write_bits(0, padding as u32)?;
        Ok(self.pos / 8)
    }
}

impl Stream for BitWriter<'_> {
    const WRITING: bool = true;

    fn serialize_bits(&mut self, value: &mut u32, bits: u32) -> Result<(), Error> {
        self.write_bits(*value, bits)
    }

    fn bit_pos(&self) -> usize {
        self.pos
    }
}

/// Reads bits from a byte slice. Every read is bounds checked.
#[derive(Debug)]
pub struct BitReader<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> BitReader<'a> {
    /// A reader over all of `buf`.
    #[must_use]
    pub fn new(buf: &'a [u8]) -> Self {
        Self { buf, pos: 0 }
    }

    /// Current position in bits.
    #[must_use]
    pub fn pos(&self) -> usize {
        self.pos
    }

    /// Bits left to read.
    #[must_use]
    pub fn bits_left(&self) -> usize {
        self.buf.len() * 8 - self.pos
    }

    /// Reads `bits` bits (`<= 32`), LSB first.
    ///
    /// # Errors
    /// [`Error::Truncated`] past the end, [`Error::Usage`] for `bits > 32`.
    pub fn read_bits(&mut self, bits: u32) -> Result<u32, Error> {
        if bits > 32 {
            return Err(Error::Usage);
        }
        if bits as usize > self.bits_left() {
            return Err(Error::Truncated);
        }
        let mut value: u64 = 0;
        let mut shift = 0;
        let mut remaining = bits as usize;
        while remaining > 0 {
            let index = self.pos / 8;
            let offset = self.pos % 8;
            let take = (8 - offset).min(remaining);
            let mask = ((1u16 << take) - 1) as u8;
            let byte = *self.buf.get(index).ok_or(Error::Truncated)?;
            value |= u64::from((byte >> offset) & mask) << shift;
            shift += take;
            remaining -= take;
            self.pos += take;
        }
        Ok(value as u32)
    }

    /// Checks that only the zero padding of the last byte remains.
    ///
    /// # Errors
    /// [`Error::TrailingData`] if a whole byte or any set bit remains.
    pub fn finish(mut self) -> Result<(), Error> {
        let left = self.bits_left();
        if left >= 8 {
            return Err(Error::TrailingData);
        }
        if self.read_bits(left as u32)? != 0 {
            return Err(Error::TrailingData);
        }
        Ok(())
    }
}

impl Stream for BitReader<'_> {
    const WRITING: bool = false;

    fn serialize_bits(&mut self, value: &mut u32, bits: u32) -> Result<(), Error> {
        *value = self.read_bits(bits)?;
        Ok(())
    }

    fn bit_pos(&self) -> usize {
        self.pos
    }
}

/// Counts the bits a value would take, without writing them.
#[derive(Debug, Default, Clone, Copy)]
pub struct Measure {
    bits: usize,
}

impl Measure {
    /// A counter at zero.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Bits counted so far.
    #[must_use]
    pub fn bits(&self) -> usize {
        self.bits
    }
}

impl Stream for Measure {
    const WRITING: bool = true;

    fn serialize_bits(&mut self, value: &mut u32, bits: u32) -> Result<(), Error> {
        if bits > 32 || (bits < 32 && (*value >> bits) != 0) {
            return Err(Error::OutOfRange);
        }
        self.bits += bits as usize;
        Ok(())
    }

    fn bit_pos(&self) -> usize {
        self.bits
    }
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used)]
    use super::*;

    #[test]
    fn lsb_first_layout() {
        let mut buf = [0xFFu8; 2];
        let mut w = BitWriter::new(&mut buf);
        w.write_bits(0b101, 3).unwrap();
        w.write_bits(1, 1).unwrap();
        w.write_bits(0xAB, 8).unwrap();
        assert_eq!(w.finish().unwrap(), 2);
        assert_eq!(buf, [0b1011_1101, 0b0000_1010]);
    }

    #[test]
    fn varuint_boundaries() {
        for value in [0, 1, 63, 64, 4095, 4096, u32::MAX >> 1, u32::MAX] {
            let mut buf = [0u8; 8];
            let mut w = BitWriter::new(&mut buf);
            let mut v = value;
            w.varuint(&mut v).unwrap();
            let len = w.finish().unwrap();
            let mut r = BitReader::new(&buf[..len]);
            let mut out = 0;
            r.varuint(&mut out).unwrap();
            assert_eq!(out, value);
            r.finish().unwrap();
        }
    }

    #[test]
    fn varuint_rejects_overlong() {
        // Group 1, more; group 0, stop: overlong form of 1.
        let mut buf = [0u8; 2];
        let mut w = BitWriter::new(&mut buf);
        w.write_bits(1, 6).unwrap();
        w.write_bits(1, 1).unwrap();
        w.write_bits(0, 6).unwrap();
        w.write_bits(0, 1).unwrap();
        w.finish().unwrap();
        let mut r = BitReader::new(&buf);
        let mut out = 0;
        assert_eq!(r.varuint(&mut out), Err(Error::NonCanonical));
    }

    #[test]
    fn zigzag_round_trip() {
        for value in [0, -1, 1, -2, i32::MIN, i32::MAX] {
            assert_eq!(zigzag_decode(zigzag_encode(value)), value);
        }
        assert_eq!(zigzag_encode(-1), 1);
        assert_eq!(zigzag_encode(1), 2);
    }
}
