# MAM Volume Identifier length correction

Release20 accepts the specified0–32-byte ASCII value for MAM attribute `0x0008`
(Volume Identifier). The maximum length is not a required fixed length.
The parser copies exactly the declared value and terminates the result there;
it never consumes bytes belonging to a following attribute. Wrong IDs,
oversized lengths and transport failures remain errors. Every other supported
MAM attribute retains its existing exact-length validation.

The [HP LTO-6 Host Interface Guide](https://docs.oracle.com/cd/E38452_01/en/LTO6_Vol3_E1_D20/LTO6_Vol3_E1_D20.pdf#page=145),
printed page145, lists `0008h Volume identifier 0–32 ASCII`.
The captured successful READ ATTRIBUTE response on a previously verified cartridge begins with
descriptor `00 08 81 00 00`: its empty value is valid, not damaged MAM.

This field is not the LTFS label or UUID and is not used as the physical
cartridge serial in receipts. Physical continuity still uses read-only MAM
`0x0401` Medium Serial Number. No setter, media-write command, index-repair
behavior, capacity calculation, or read-only policy is changed.
This warning alone does not require rewriting existing cartridges.

## Verification scope

The real C parser is exercised through a fake device backend with every valid
length0–32, oversized lengths33 and65535, wrong IDs, transport errors, and
representative fixed-size serial/barcode/vendor fields. Assertions also prove
that destination bytes after the actual value remain untouched.

- RED: the previous driver rejected valid lengths0–31.
- GREEN: all35 driver tests passed, with no skips.
- ASan/UBSan: all35 tests passed, with no skips; LeakSanitizer is disabled by
  the existing qualification script and is not claimed as verified.
- Packaging and provenance checks must run from a clean source tree, not an
  in-place compiled tree. Generated files are correctly rejected by provenance.

Release20 was deployed with application134/runtime3 on 2026-09-08. A bounded
read-only session on that cartridge confirmed that a queried file was absent,
index generation remained2, and native unmount and
device close completed with result0. No backup payload files were reread.
The native process-tree ioctl trace contained10 READ commands and no forbidden
media-write commands; the offline application database families were unchanged.
Services were restored with the backup still paused.

This is not a full-tape integrity or cross-system interoperability result.
The same session exposed a separate close-time SG MODE SELECT length defect;
successful best-effort close does not imply every auxiliary SCSI command
succeeded. That defect is tracked independently of the MAM parser correction.
