-- Logistics entity timeline recipe.

SELECT timestamp, state, zone
FROM pallet_events
WHERE pallet_id = 10042
ORDER BY timestamp ASC;

SELECT a.agv_id, a.timestamp, a.lat, a.lon
FROM agv_pose a
WHERE ST_Within(a.lat, a.lon, 37.7749, -122.4194, 50)
ORDER BY a.timestamp ASC;
