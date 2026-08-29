% revival-fog project rules (loaded after sofar core.lp; DESIGN.md §5).
% 1. floors: which laws bound a measurement's magnitude (F7, the code-placement floor)
floor(F) :- kind(F,law), subject(F,"perf.floor").
% 2. detector validity vs the labeled corpus (F6): a detector that reads quiet on a state
%    Gil-Ad labeled broken, or fires on one he labeled ok, is in conflict with its validity claim
reading(R,Det,St,V) :- current(R), kind(R,measurement), subject(R,"detector.reading"),
                       scope(R,detector,Det), scope(R,state,St), value(R,V).
label(L,St,V)       :- current(L), kind(L,verdict), subject(L,"greets.wall.look"),
                       scope(L,state,St), value(L,V).
validity(D,Det)     :- current(D), kind(D,law), subject(D,"detector.valid"), scope(D,detector,Det).
conflict(det(D,R,L)) :- validity(D,Det), reading(R,Det,St,"quiet"), label(L,St,V), V != "ok".
conflict(det(D,R,L)) :- validity(D,Det), reading(R,Det,St,"fires"), label(L,St,"ok").
member(det(D,R,L),D) :- conflict(det(D,R,L)).
member(det(D,R,L),R) :- conflict(det(D,R,L)).
member(det(D,R,L),L) :- conflict(det(D,R,L)).
