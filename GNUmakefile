
OBJ = o/main.a o/connection.a


dict : $(OBJ)
	iix link o/main o/connection keep=$@


o/main.a : main.c connection.h
o/connection.a : connection.c connection.h


o :
	mkdir $@

o/%.a : %.c | o
	iix compile $< keep=o/$*
