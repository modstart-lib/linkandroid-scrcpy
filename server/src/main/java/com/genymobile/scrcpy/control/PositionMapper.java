package com.genymobile.scrcpy.control;

import com.genymobile.scrcpy.device.Point;
import com.genymobile.scrcpy.device.Position;
import com.genymobile.scrcpy.device.Size;
import com.genymobile.scrcpy.util.AffineMatrix;

public final class PositionMapper {

    private final Size videoSize;
    private final AffineMatrix videoToDeviceMatrix;

    public PositionMapper(Size videoSize, AffineMatrix videoToDeviceMatrix) {
        this.videoSize = videoSize;
        this.videoToDeviceMatrix = videoToDeviceMatrix;
    }

    public static PositionMapper create(Size videoSize, AffineMatrix filterTransform, Size targetSize) {
        boolean convertToPixels = !videoSize.equals(targetSize) || filterTransform != null;
        AffineMatrix transform = filterTransform;
        if (convertToPixels) {
            AffineMatrix inputTransform = AffineMatrix.ndcFromPixels(videoSize);
            AffineMatrix outputTransform = AffineMatrix.ndcToPixels(targetSize);
            transform = outputTransform.multiply(transform).multiply(inputTransform);
        }

        return new PositionMapper(videoSize, transform);
    }

    public Size getVideoSize() {
        return videoSize;
    }

    public Point map(Position position) {
        Size clientVideoSize = position.getScreenSize();
        if (!videoSize.equals(clientVideoSize)) {
            // The client sends a click relative to a video with wrong dimensions
            // Instead of ignoring, scale the coordinates to match current video size
            Point originalPoint = position.getPoint();
            
            float scaleX = (float) videoSize.getWidth() / clientVideoSize.getWidth();
            float scaleY = (float) videoSize.getHeight() / clientVideoSize.getHeight();
            
            int scaledX = Math.round(originalPoint.getX() * scaleX);
            int scaledY = Math.round(originalPoint.getY() * scaleY);
            
            if (com.genymobile.scrcpy.util.Ln.isEnabled(com.genymobile.scrcpy.util.Ln.Level.VERBOSE)) {
                com.genymobile.scrcpy.util.Ln.v("PositionMapper: scaled " + clientVideoSize + " -> " + videoSize + 
                                               " point(" + originalPoint.getX() + "," + originalPoint.getY() + ")" +
                                               " -> (" + scaledX + "," + scaledY + ")");
            }
            
            Point scaledPoint = new Point(scaledX, scaledY);
            
            if (videoToDeviceMatrix != null) {
                scaledPoint = videoToDeviceMatrix.apply(scaledPoint);
            }
            
            return scaledPoint;
        }

        Point point = position.getPoint();
        if (videoToDeviceMatrix != null) {
            point = videoToDeviceMatrix.apply(point);
        }
        return point;
    }
}
