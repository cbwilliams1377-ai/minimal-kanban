ffmpeg -i check-square.png -vf scale=256:256 check-square.ico
windres minimal_kanban.rc minimal_kanban-res.o
g++ minimal_kanban.cpp minimal_kanban-res.o -o MinimalKanban.exe -std=c++17 -O2 -s -mwindows -static -static-libgcc -static-libstdc++ -municode -lurlmon -lole32 -lshell32 -lgdi32 -luser32 -luuid