package org.servalproject.servaldna.meshmb;

import java.util.Date;

/**
 * Created by jeremy on 10/10/16.
 */
public class PlyMessage implements Comparable<PlyMessage>{

    public final long _row;
    public final long offset;
    public final String token;
    public final long timestamp;
    public final Date date;
    public final String text;

    public PlyMessage(long _row, long offset, String token, long timestamp, String text){
        this._row = _row;
        this.offset = offset;
        this.token = token;
        this.timestamp = timestamp;
        this.date = new Date(timestamp * 1000);
        this.text = text;
    }

    @Override
    public int compareTo(PlyMessage plyMessage) {
        if (this.offset == plyMessage.offset)
            return 0;
        return (this.offset < plyMessage.offset) ? 1 : -1;
    }
}
