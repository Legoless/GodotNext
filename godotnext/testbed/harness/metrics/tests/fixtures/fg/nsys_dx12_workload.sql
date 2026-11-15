-- A hand-written stand-in for an "nsys export --type=sqlite" report (#202 F1 fixture). It has only
-- the tables and columns nsys_dlssg.py reads, under the names the Nsight Systems documentation
-- gives for the export: DX12_WORKLOAD's start, end, frameId, objectNameId and textId, and
-- StringIds' id and value. A real export has more columns; the rows are invented. Times are ns.
-- Frames 1-6 each have one GodotNext workload and DLSS-G work on the Streamline queue: frame 3's
-- two DLSS-G workloads overlap (union 1.5 ms), frame 4's are disjoint (1.0 ms). One DLSS-G
-- workload has no frame index, and one Streamline workload that is not DLSS-G is named
-- nv.sl.reflex.
CREATE TABLE StringIds (id INTEGER PRIMARY KEY, value TEXT);
CREATE TABLE DX12_WORKLOAD (start INTEGER, end INTEGER, frameId INTEGER, objectNameId INTEGER, textId INTEGER);
INSERT INTO StringIds (id, value) VALUES
    (1, 'ExecuteCommandLists'),
    (2, 'nv.sl.dlss_g.cmdQ.dlssg'),
    (3, 'GodotNext graphics queue'),
    (4, 'nv.sl.reflex.cmdQ');
INSERT INTO DX12_WORKLOAD (start, end, frameId, objectNameId, textId) VALUES
    (1000000, 9000000, 1, 3, NULL),
    (9100000, 10300000, 1, 2, NULL),
    (17000000, 25000000, 2, 3, NULL),
    (25100000, 26300000, 2, 2, NULL),
    (33000000, 41000000, 3, 3, NULL),
    (41000000, 42000000, 3, 2, NULL),
    (41500000, 42500000, 3, 2, NULL),
    (49000000, 57000000, 4, 3, NULL),
    (57000000, 57400000, 4, 2, NULL),
    (57500000, 58100000, 4, 2, NULL),
    (65000000, 73000000, 5, 3, NULL),
    (73100000, 74400000, 5, 2, NULL),
    (81000000, 89000000, 6, 3, NULL),
    (89100000, 90200000, 6, 2, NULL),
    (90300000, 90500000, 6, 4, NULL),
    (95000000, 96000000, NULL, 2, NULL);
