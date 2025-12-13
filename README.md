# K1 & K5/v3 FW

## Build

### FW

```sh
make clean && make release
```

### k5prog

```sh
g++ k5prog.c -o k5prog
```

## Flash

```sh 
./k5prog/k5prog -F -YYYYY -b ./bin/firmware.bin
```

## Credits

[Muzkr](https://github.com/muzkr) - thanks for [PY32F071 LL library compilation and boilerplate](https://github.com/armel/uv-k1-k5v3-firmware-custom)

[DualTachyon](https://github.com/DualTachyon) - father of UV-K5 codebase


