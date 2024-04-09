# This Branch aim to add support of the MABR with flute protocol.

This Documment is an attempt to conceptualize and determin what is needed to be done to add support for flute.   

# Functionalities:
for now we are using the route module and implementing the flute protocol inside the route filters as an option. this could be changed later if it is more accurate to seperate the two protocols filters. as a result the files that will be modified are route.h. in_route.c, out_route.c and route_dmx.c 

##  Reciever (pcap or live session)
Files to be changed : in_route.c , route_dmx.c :
### in_route : 
   [Done] add Flute mode. "flute://" will be used to indicate that flute will be used. 
   [TODO] Potentialy segregate the flute functions and the route functions.  

### route.dmx:
   [Done] READ Packets and decode LCT HEADers, 
   [Done] READ EXT_FTD and EXT_FTI Headers. 
   [Done] ADD FDT table representation structs.  
   [Done] use of ESI to reconcstruct objects.  
   [Done] use of total length instead ot tol_size to reconstruct objects. 
   [TODO] parsing FDT, and FDT instance creation
##  Sender 