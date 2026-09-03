# QoiShellExt

Shell extensions in-proc para imagens [QOI](https://qoiformat.org/): thumbnail
provider e preview handler para arquivos `.qoi`.

*[Read in English](README.md)*

## O que faz

- **Miniaturas** no File Explorer, em qualquer tamanho de ícone.
- **Preview pane**, escalando para caber, com o fundo do próprio painel e um
  xadrez atrás da transparência.

Os dois rodam dentro do host do próprio shell (`dllhost.exe` e `prevhost.exe`).
Não há executável auxiliar para disparar, nem arquivo temporário, nem runtime
gerenciado para subir - por isso uma miniatura custa cerca de meio milissegundo,
em vez do tempo de iniciar um processo.

Os dois handlers são independentes: instale um, o outro, ou ambos.

## Medido

| arquivo | tamanho | alvo | tempo |
| --- | --- | --- | --- |
| imagem pequena | 520x256 | thumbnail de 256 px | 0,49 ms |
| imagem pequena | 520x256 | preview pane de 500x400 | 0,49 ms (`DoPreview`) |
| gradiente 4000x3000 (18 MB) | 12 Mpx | thumbnail de 256 px | 79 ms |
| gradiente 4000x3000 (18 MB) | 12 Mpx | thumbnail de 1024 px | 88 ms |

O caso grande é dominado pelo decode do QOI, que é sequencial por natureza -
não dá para decodificar só uma faixa da imagem.

## Como funciona

Uma DLL, dois CLSIDs:

| | CLSID | shellex |
| --- | --- | --- |
| Thumbnail provider | `{7BF3DDA8-F6F6-49E0-8F62-7B15E312302C}` | `{E357FCCD-...}` |
| Preview handler | `{BDB2367F-2BC8-4503-A870-AF02BD359A65}` | `{8895b1c6-...}` |

- `QoiImage.cpp` - decode pelo `qoi.h` de referência (`QOI_NO_STDIO`, forçando
  4 canais) e escalonamento: box filter ao reduzir, bilinear ao ampliar, os dois
  ponderando a cor pelo alpha - senão a cor (arbitrária) dos pixels
  transparentes vaza para a borda e vira halo escuro.
- `ThumbnailProvider.cpp` - `IInitializeWithStream` + `IThumbnailProvider`,
  escrevendo direto num DIB section de 32 bpp top-down. Nunca amplia; devolve
  `WTSAT_ARGB` (alpha não pré-multiplicado, que é o que o QOI guarda).
  `ThreadingModel = Both`, para o host de thumbnails não precisar marshalar.
- `PreviewHandler.cpp` - `IPreviewHandler` + `IPreviewHandlerVisuals` +
  `IOleWindow` + `IObjectWithSite` sobre uma janela filha comum. Escala para
  caber (ampliando quando a imagem é pequena), respeita as cores de fundo e de
  texto do painel, desenha um xadrez atrás da transparência, faz cache do bitmap
  escalado por tamanho de painel e usa double buffering para não piscar ao
  arrastar o divisor. `ThreadingModel = Apartment`, com `AppID` apontando para o
  surrogate `prevhost.exe`.
- `ShellExt.cpp` - class factory, `DllRegisterServer` / `DllUnregisterServer` e
  `DllInstall`, que é o que viabiliza instalar um handler de cada vez.

Dependências: só Win32 (`gdi32`, `msimg32`, `ole32`, `advapi32`, `shlwapi`,
`shell32`). Compilado com `/MT`, então não precisa do VC++ redistributable.

## Build

```bat
build.cmd
```

Gera `build\QoiShellExt.dll` (x64). Precisa do Visual Studio 2022 Community no
caminho padrão; se estiver em outro lugar, ajuste `VSDEV` no script.

`build.cmd thumbnail` e `build.cmd preview` compilam uma DLL que carrega só
aquele handler. Raramente é necessário: o build padrão tem os dois, e quais
serão *registrados* se decide na instalação.

## Instalação

Baixe o zip na página de Releases, extraia e rode `install.cmd` como
administrador - sem compilador, sem build. Os mesmos scripts funcionam direto
da árvore de fontes depois do `build.cmd`.

```bat
install.cmd             (como administrador - os dois handlers)
install.cmd thumbnail   (só miniaturas)
install.cmd preview     (só o preview pane)

uninstall.cmd           (remove tudo)
uninstall.cmd preview   (remove só aquele handler)
```

Instala em `%ProgramFiles%\navossoc\QoiShellExt`. Acrescentar o outro handler
depois é outro `install.cmd`, sem rebuild.

Por baixo é `regsvr32 /n /i:<handler>`, que chama `DllInstall` em vez de
`DllRegisterServer`. Um `regsvr32 QoiShellExt.dll` puro continua instalando tudo
que a DLL carrega.

Se já houver outro thumbnail ou preview handler registrado para `.qoi`,
desligue-o antes - muitos se re-registram no startup e retomam a associação.

Para ver o efeito, limpe o cache de miniaturas:

```bat
del /q "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db"
```

## Teste

```bat
test\build-test.cmd
test\build-gen.cmd
cd build
gen.exe sample.qoi 640 480
test.exe thumb   sample.qoi 256 200
test.exe preview sample.qoi 500 400
```

O `test.exe` carrega a DLL sem registrar no COM, exercita o handler e grava
`out.bmp` para conferência. O modo preview pinta via `WM_PRINTCLIENT`, então
nenhuma janela precisa estar visível.

O `gen.exe` escreve um `.qoi` de qualquer tamanho; passe algo como `4000 3000`
para medir o custo numa imagem de tamanho real.

## Release

```bat
package.cmd
```

Compila os dois handlers e monta `dist\QoiShellExt-<versão>-x64.zip` com a
DLL, os dois scripts de instalação, a licença e os READMEs, e imprime o SHA256
do artefato para as notas da release. A versão sai do `VER_STRING` em
`QoiShellExt.rc`, então é o único lugar a incrementar.

## Licença

MIT - veja [LICENSE](LICENSE). Embute o `qoi.h` de Dominic Szablewski, também
MIT.
