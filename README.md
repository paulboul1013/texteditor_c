# text_editor

# introduce

interface like vi/nano simple text editor 

# feature

- single file or multiple files view and edit(can use Ctrl + ←/→ switch between files)

- basic operation:up and down move、insert new line、delete line、edit line、copy&paste line

- finding mode (can find target string and then high light that target string)

- live share with many person edit to file same time(most 20 persons can edit，have cursor show)


# build

```bash
make
```

# execute

```bash
./main <filename1> [filename2]
```

# keyboard operation 

- main view
    - ↑/↓：up and down move to choose line
    - Enter:into target line，start edit mode to edit text
    - f:into finding mode ，find target key word and then will highlight text，press n will find next 
    - n:insert new empty line
    - c:copy line  text and paste to clipboard
    - p:paste clipboard to line
    - Ctrl+←/→:switch files view
    - q:quit texteditor(autosave)

- editor mode(edit one line)
    - ←/→：move cursor 
    - Backspace:delete before cursor one character
    - Enter:make sure your edited line to save
    - ESC:cancel edit and move back main view

- window show
    - show green color light ">>>[行 N]"

- auto save
  - after complete operation will auto save file
  - save again after exit texteditor


# Live Share ()

Built-in tcp P2S (multiple client to single host) online cooperate at most 20 persons，support all client show user id and highlight cursor  


## usage
 - host mode
 - 
```bash
./main --host 5555 <filename1> [filename2]
```

- join mode（connect to host）

```bash
./main --join 127.0.0.1:5555 <filename1> [filename2]
```

# to-do

- 網路通訊未加密、未驗證
- 終端尺寸變更未特別處理

# reference
https://www.youtube.com/watch?v=gnvDPCXktWQ  
https://github.com/mattduck/kilo/tree/master  
