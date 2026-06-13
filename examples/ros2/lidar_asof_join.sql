-- Fuse odometry pose with the most recent LaserScan summary.

SELECT o.timestamp,
       o.pose_position_x,
       o.pose_position_y,
       l.ranges_min,
       l.ranges_mean,
       l.ranges_max
FROM ros_odometry o
ASOF JOIN ros_laserscan l
ON o.symbol = l.symbol AND o.timestamp >= l.timestamp
WHERE o.symbol = 900
ORDER BY o.timestamp ASC;
