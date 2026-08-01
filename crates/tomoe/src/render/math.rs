//! Small column-major 2D linear algebra used by the shader uniforms.

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Vec2 {
    pub x: f32,
    pub y: f32,
}

impl Vec2 {
    pub const fn new(x: f32, y: f32) -> Self {
        Self { x, y }
    }
}

impl std::ops::Sub for Vec2 {
    type Output = Self;

    fn sub(self, rhs: Self) -> Self::Output {
        Self::new(self.x - rhs.x, self.y - rhs.y)
    }
}

impl std::ops::Div for Vec2 {
    type Output = Self;

    fn div(self, rhs: Self) -> Self::Output {
        Self::new(self.x / rhs.x, self.y / rhs.y)
    }
}

impl std::ops::Neg for Vec2 {
    type Output = Self;

    fn neg(self) -> Self::Output {
        Self::new(-self.x, -self.y)
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Mat3 {
    cols: [f32; 9],
}

impl Mat3 {
    pub const IDENTITY: Self = Self {
        cols: [1., 0., 0., 0., 1., 0., 0., 0., 1.],
    };

    pub const fn from_cols_array(cols: &[f32; 9]) -> Self {
        Self { cols: *cols }
    }

    pub const fn to_cols_array(self) -> [f32; 9] {
        self.cols
    }

    pub fn from_translation(v: Vec2) -> Self {
        Self::from_cols_array(&[1., 0., 0., 0., 1., 0., v.x, v.y, 1.])
    }

    pub fn from_scale(v: Vec2) -> Self {
        Self::from_cols_array(&[v.x, 0., 0., 0., v.y, 0., 0., 0., 1.])
    }
}

impl AsRef<[f32; 9]> for Mat3 {
    fn as_ref(&self) -> &[f32; 9] {
        &self.cols
    }
}

impl std::ops::Mul for Mat3 {
    type Output = Self;

    fn mul(self, rhs: Self) -> Self::Output {
        let mut out = [0.; 9];
        // Arrays are column-major: index 6/7 is the translation column.
        for col in 0..3 {
            for row in 0..3 {
                out[col * 3 + row] = (0..3)
                    .map(|k| self.cols[k * 3 + row] * rhs.cols[col * 3 + k])
                    .sum();
            }
        }
        Self::from_cols_array(&out)
    }
}

#[cfg(test)]
mod tests {
    use super::{Mat3, Vec2};

    #[test]
    fn column_major_layout_and_products() {
        assert_eq!(
            Mat3::IDENTITY.to_cols_array(),
            [1., 0., 0., 0., 1., 0., 0., 0., 1.]
        );
        assert_eq!(
            Mat3::from_translation(Vec2::new(3., 5.)).to_cols_array(),
            [1., 0., 0., 0., 1., 0., 3., 5., 1.]
        );
        assert_eq!(
            Mat3::from_scale(Vec2::new(2., 4.)).to_cols_array(),
            [2., 0., 0., 0., 4., 0., 0., 0., 1.]
        );

        let translate = Mat3::from_translation(Vec2::new(3., 5.));
        let scale = Mat3::from_scale(Vec2::new(2., 4.));
        assert_eq!(
            (translate * scale).to_cols_array(),
            [2., 0., 0., 0., 4., 0., 3., 5., 1.]
        );
        assert_eq!(
            (scale * translate).to_cols_array(),
            [2., 0., 0., 0., 4., 0., 6., 20., 1.]
        );

        let cols = [1., 2., 3., 4., 5., 6., 7., 8., 9.];
        assert_eq!(Mat3::from_cols_array(&cols).to_cols_array(), cols);
    }
}
