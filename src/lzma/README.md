# LZMA2 codec sources

The C sources in this directory are the public-domain LZMA/LZMA2 encoder and
decoder components from the 7-Zip SDK. They are used only for the SX V2 raw
LZMA2 streams; the SX V1 codec and OFDM wire format are independent.

The copied revision is 7-Zip 26.02, commit
`f9d78aff31a5f2521ae7ddbdc97c4a8855808959`. Each copied source file carries
Igor Pavlov's public-domain notice. The adapter files in `src/lzma2_*.c` are
project code.
