
BIN_OBJ = o/main.a o/connection.a
NDA_OBJ = o/nda.a o/connection.a o/tools.a

dict : $(BIN_OBJ)
	iix link o/main o/connection keep=$@


dict.nda : $(NDA_OBJ) o/nda.r
	iix link o/nda o/connection o/tools keep=$@
	iix copyfork o/nda.r $@ -r
	iix chtyp -t nda $@

o/main.a : main.c connection.h
o/connection.a : connection.c connection.h

o/tools.a : tools.c
o/nda.a : nda.c nda.h
o/nda.r : nda.rez nda.h

o :
	mkdir $@

o/%.a : %.c | o
	iix compile $< keep=o/$*

o/%.r : %.rez | o
	iix compile $< keep=$@
