# IME3 Dictionary Format

This documents the binary dictionary format consumed by `main/ime/yong_dict.*`.
All integer fields are little-endian.

## Header

Offset | Size | Meaning
--- | --- | ---
0 | 4 | Magic: `IME3`
4 | 1 | Scheme: `0` Wubi, `1` Pinyin, `2` Shuangpin
5 | 1 | Single-character code length, 1..6
6 | 2 | Reserved
8 | 4 | Single-character record count

After the 12-byte header there are `26 * 26 + 1` 32-bit index entries.
Indexes are keyed by the first two lowercase ASCII code letters. For one-letter
windows, the range is `index[c0 * 26]..index[(c0 + 1) * 26]`.

## Single-Character Records

Single records start after the header index.

Field | Size | Meaning
--- | --- | ---
code | `code_len` | NUL-padded ASCII code
text | 3 | UTF-8 Hanzi bytes
flag | 1 | Visibility flags

Flag bit `0x01` hides an entry in traditional mode. Flag bit `0x02` hides an
entry in simplified mode.

## Phrase Section

The phrase section starts immediately after the single-character records.

Field | Size | Meaning
--- | --- | ---
word_count | 4 | Phrase group count
word_index | `(26 * 26 + 1) * 4` | Byte offsets into phrase data
word_data | variable | Phrase groups

Each phrase group in `word_data` is:

Field | Size | Meaning
--- | --- | ---
code_len | 1 | ASCII code byte length
code | `code_len` | ASCII code
candidate_count | 1 | Number of candidates
candidate | variable | Repeated candidate records

Each phrase candidate is:

Field | Size | Meaning
--- | --- | ---
word_len | 1 | UTF-8 byte length
word | `word_len` | UTF-8 text
flag | 1 | Same visibility flags as single records

`word_index[26 * 26]` is the total `word_data` size.

## Prediction Section

If non-padding bytes remain after phrase data, they are parsed as predictions:

Field | Size | Meaning
--- | --- | ---
predict_count | 4 | Prediction group count
predict_data | variable | Prediction groups

Each prediction group is:

Field | Size | Meaning
--- | --- | ---
key | 1 UTF-8 char | CJK key character
candidate_count | 1 | Number of prediction candidates
candidate | variable | Repeated prediction text

Each prediction candidate is:

Field | Size | Meaning
--- | --- | ---
word_len | 1 | UTF-8 byte length
word | `word_len` | UTF-8 text

The runtime builds a lazy in-memory `hash + offset` index for prediction
groups. If `predict_count` is greater than `PJOURNAL_IME_PREDICT_INDEX_MAX_GROUPS`,
the runtime falls back to linear scanning to cap memory use.

## Compatibility Notes

The current format has no explicit section length for predictions and no version
field beyond the `IME3` magic. New sections should therefore be added only after
an explicit version or tagged trailer is introduced, otherwise old firmware may
interpret the extra bytes as prediction data.
